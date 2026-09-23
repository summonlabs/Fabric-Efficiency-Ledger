// Fabric Efficiency Ledger - bounded concurrency, cancellation, and lock auditing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fel/concurrency.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fel {
namespace {

// ---------------------------------------------------------------------------
// Lock audit internals.
//
// The audit keeps everything it needs in atomics and thread_local storage on
// purpose: there is no audit mutex, so auditing an acquisition can never be the
// reason a program blocks or deadlocks. The check runs while the mutex is held
// so the recorded stack always describes locks the thread really owns.
// ---------------------------------------------------------------------------
struct HeldLock {
  const CheckedMutex* mutex = nullptr;
  LockRank rank = LockRank::Registry;
};

std::vector<HeldLock>& held_lock_stack() {
  thread_local std::vector<HeldLock> stack;
  return stack;
}

// Lock free "publish if greater". It stops as soon as it wins the race and only
// iterates again when another thread has just published a larger value, so the
// work is bounded by the number of concurrent writers.
void note_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) noexcept {
  std::uint64_t observed = peak.load(std::memory_order_relaxed);
  while (observed < value) {
    if (peak.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return;
    }
  }
}

std::uint32_t clamp_workers(std::uint32_t workers) noexcept {
  if (workers == 0U) {
    return 1U;
  }
  if (workers > limits::kMaxWorkers) {
    return limits::kMaxWorkers;
  }
  return workers;
}

std::size_t clamp_queue_depth(std::size_t depth) noexcept {
  if (depth == 0U) {
    return 1U;
  }
  if (depth > limits::kMaxQueueDepth) {
    return limits::kMaxQueueDepth;
  }
  return depth;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lock audit
// ---------------------------------------------------------------------------
LockAuditCounters& lock_audit() noexcept {
  static LockAuditCounters counters;
  return counters;
}

LockAuditCountersSnapshot lock_audit_snapshot() noexcept {
  const LockAuditCounters& counters = lock_audit();
  LockAuditCountersSnapshot snapshot;
  snapshot.acquisitions = counters.acquisitions.load(std::memory_order_relaxed);
  snapshot.reentrancy_detections = counters.reentrancy_detections.load(std::memory_order_relaxed);
  snapshot.lock_order_violations = counters.lock_order_violations.load(std::memory_order_relaxed);
  return snapshot;
}

void reset_lock_audit() noexcept {
  LockAuditCounters& counters = lock_audit();
  counters.acquisitions.store(0, std::memory_order_relaxed);
  counters.reentrancy_detections.store(0, std::memory_order_relaxed);
  counters.lock_order_violations.store(0, std::memory_order_relaxed);
}

std::string_view lock_rank_name(LockRank rank) noexcept {
  switch (rank) {
    case LockRank::Registry:
      return "Registry";
    case LockRank::StoreState:
      return "StoreState";
    case LockRank::Segment:
      return "Segment";
    case LockRank::Index:
      return "Index";
    case LockRank::Diagnostics:
      return "Diagnostics";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// CheckedMutex
// ---------------------------------------------------------------------------
CheckedMutex::Guard CheckedMutex::acquire(const char* site) {
  mutex_.lock();

  LockAuditCounters& audit = lock_audit();
  audit.acquisitions.fetch_add(1, std::memory_order_relaxed);

  bool reentrant = false;
  bool order_violation = false;
  std::vector<HeldLock>& stack = held_lock_stack();
  for (const HeldLock& held : stack) {
    if (held.mutex == this) {
      // The mutex is already held by this thread. A plain std::mutex cannot be
      // locked twice, so reaching here means the caller is about to deadlock;
      // the counter and the guard flag are how that defect becomes visible.
      reentrant = true;
    }
    // Lower rank means "outer". Acquiring a mutex that ranks outside one this
    // thread already holds is the lock order defect (inner held, outer wanted).
    if (static_cast<std::uint32_t>(held.rank) > static_cast<std::uint32_t>(rank_)) {
      order_violation = true;
    }
  }

  if (reentrant) {
    audit.reentrancy_detections.fetch_add(1, std::memory_order_relaxed);
  }
  if (order_violation) {
    audit.lock_order_violations.fetch_add(1, std::memory_order_relaxed);
  }

  try {
    stack.push_back(HeldLock{this, rank_});
  } catch (...) {
    // The audit could not record the lock: give the mutex back rather than
    // leaving it locked with no owner on the stack.
    mutex_.unlock();
    throw;
  }
  return Guard(this, reentrant, order_violation, site);
}

bool CheckedMutex::held_by_current_thread() const noexcept {
  const std::vector<HeldLock>& stack = held_lock_stack();
  for (const HeldLock& held : stack) {
    if (held.mutex == this) {
      return true;
    }
  }
  return false;
}

void CheckedMutex::Guard::release() noexcept {
  CheckedMutex* owner = owner_;
  if (owner == nullptr) {
    return;  // Already released, or moved from: nothing is owned any more.
  }
  owner_ = nullptr;
  reentrant_ = false;
  order_violation_ = false;
  site_ = nullptr;

  // Pop before unlocking so the stack never claims a lock that is already free,
  // and so a waiting thread can never observe a stale owner.
  std::vector<HeldLock>& stack = held_lock_stack();
  for (std::size_t index = stack.size(); index > 0U; --index) {
    if (stack[index - 1U].mutex == owner) {
      stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index - 1U));
      break;
    }
  }
  owner->mutex_.unlock();
}

// ---------------------------------------------------------------------------
// Worker budget
// ---------------------------------------------------------------------------
std::uint32_t hardware_worker_budget() noexcept {
  const unsigned int hardware = std::thread::hardware_concurrency();
  const std::uint32_t reported = hardware == 0U ? 1U : static_cast<std::uint32_t>(hardware);
  return clamp_workers(reported);
}

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------
ThreadPool::ThreadPool(PoolConfig config)
    : config_(std::move(config)),
      queue_(clamp_queue_depth(config_.queue_depth)),
      cancel_(),
      threads_(),
      stats_mutex_(),
      stats_(),
      running_(false),
      in_flight_(0),
      peak_queue_(0),
      lifecycle_mutex_() {}

ThreadPool::~ThreadPool() { shutdown(ShutdownMode::Abort); }

Result<void> ThreadPool::start() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (running_.load(std::memory_order_acquire) || !threads_.empty()) {
    return make_error(ErrorCode::InvalidArgument, "thread pool has already been started");
  }
  if (queue_.closed() || queue_.aborted()) {
    // shutdown() is terminal: the queue can never accept work again, so
    // restarting would only produce workers that exit immediately.
    return make_error(ErrorCode::InvalidArgument, "thread pool has already been shut down");
  }

  const std::uint32_t workers = clamp_workers(config_.workers);
  try {
    threads_.reserve(static_cast<std::size_t>(workers));
    for (std::uint32_t index = 0; index < workers; ++index) {
      threads_.emplace_back([this] { worker_loop(); });
    }
  } catch (...) {
    // Roll back rather than leaving a half started pool behind. Aborting the
    // queue is what releases the workers that were already created, since they
    // can only be waiting for work.
    queue_.abort();
    for (std::thread& worker : threads_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    threads_.clear();
    return make_error(ErrorCode::Internal, "failed to create worker threads");
  }

  // running_ is published last: submit() can only succeed once every worker
  // exists, so a failed start can never strand accepted work.
  running_.store(true, std::memory_order_release);
  return ok();
}

Result<void> ThreadPool::submit(Job job) {
  if (!running_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.rejected;
    return make_error(ErrorCode::QueueClosed, "thread pool is not running");
  }
  if (!job) {
    return make_error(ErrorCode::InvalidArgument, "job is empty");
  }
  if (cancel_.cancelled()) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.rejected;
    return make_error(ErrorCode::Cancelled, "thread pool is shutting down");
  }

  // Counted as submitted before it becomes visible to a worker, so a snapshot
  // can never report more completions than submissions.
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.submitted;
  }

  Result<void> pushed = queue_.try_push(std::move(job));
  if (!pushed.has_value()) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    --stats_.submitted;
    ++stats_.rejected;
    return pushed.error();
  }
  note_peak(peak_queue_, static_cast<std::uint64_t>(queue_.size()));
  return ok();
}

void ThreadPool::shutdown(ShutdownMode mode) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

  const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
  if (!was_running && threads_.empty()) {
    return;  // Idempotent, and safe from the destructor of a pool never started.
  }

  if (mode == ShutdownMode::Abort) {
    cancel_.cancel();
    const std::size_t dropped = queue_.size();
    queue_.abort();
    if (dropped != 0U) {
      // Work that was accepted and then dropped is cancelled work; counting it
      // keeps submitted == completed + failed + cancelled after an abort.
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.cancelled += static_cast<std::uint64_t>(dropped);
    }
  } else {
    queue_.close();  // Refuse new work, let the consumers drain what is queued.
  }

  // Joining is real: when this returns no worker thread is running.
  for (std::thread& worker : threads_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  threads_.clear();
}

void ThreadPool::worker_loop() {
  const CancellationToken token = cancel_.token();
  for (;;) {
    Result<Job> popped = queue_.pop(token);
    if (!popped.has_value()) {
      // Cancelled (aborted) or QueueClosed (closed and drained): the pool is
      // winding down and this worker is done.
      return;
    }

    Job job = std::move(popped).value();
    in_flight_.fetch_add(1, std::memory_order_acq_rel);
    note_peak(peak_queue_, static_cast<std::uint64_t>(queue_.size()));

    bool failed = false;
    try {
      job(token);
    } catch (...) {
      // Defensive: a job must never be able to kill its worker thread or
      // propagate an exception across the thread boundary.
      failed = true;
    }

    in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      if (failed) {
        ++stats_.failed;
      } else {
        ++stats_.completed;
      }
    }
  }
}

PoolStats ThreadPool::stats() const {
  PoolStats snapshot;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    snapshot = stats_;
  }
  // These two are published lock free from the atomics so that work happening
  // right now is neither missed nor counted twice, and so running() and the
  // snapshot can never disagree about the lifecycle.
  snapshot.jobs_in_flight = in_flight_.load(std::memory_order_relaxed);
  const std::uint64_t peak = peak_queue_.load(std::memory_order_relaxed);
  if (peak > snapshot.peak_queue) {
    snapshot.peak_queue = peak;
  }
  snapshot.running = running_.load(std::memory_order_acquire);
  return snapshot;
}

}  // namespace fel
