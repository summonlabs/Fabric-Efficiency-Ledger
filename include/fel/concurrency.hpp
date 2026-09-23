// Fabric Efficiency Ledger - bounded concurrency, cancellation, lock audit.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fel/checked.hpp"
#include "fel/error.hpp"
#include "fel/limits.hpp"

namespace fel {

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------
class CancellationToken {
 public:
  CancellationToken() = default;
  explicit CancellationToken(const std::shared_ptr<std::atomic<bool>>& flag) : flag_(flag) {}

  [[nodiscard]] bool cancelled() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }
  [[nodiscard]] bool valid() const noexcept { return flag_ != nullptr; }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
 public:
  CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  void cancel() noexcept { flag_->store(true, std::memory_order_release); }
  [[nodiscard]] bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }
  [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken{flag_}; }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

// ---------------------------------------------------------------------------
// Bounded queue
//
// Real blocking with a real bound. A producer that outruns the consumer blocks
// instead of growing without limit. close() lets consumers drain; abort() drops
// pending work immediately, which is what shutdown needs to be honest.
// ---------------------------------------------------------------------------
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  [[nodiscard]] Result<void> push(T value, const CancellationToken& token = CancellationToken{}) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this, &token] {
      return closed_ || aborted_ || queue_.size() < capacity_ || token.cancelled();
    });
    if (aborted_ || closed_) {
      return make_error(ErrorCode::QueueClosed, "queue is closed");
    }
    if (token.cancelled()) {
      return make_error(ErrorCode::Cancelled, "cancelled while waiting for queue space");
    }
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return ok();
  }

  // Non blocking variant used on paths that must not stall (shutdown).
  [[nodiscard]] Result<void> try_push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_ || closed_) {
      return make_error(ErrorCode::QueueClosed, "queue is closed");
    }
    if (queue_.size() >= capacity_) {
      return make_error(ErrorCode::QueueFull, "queue is at capacity");
    }
    queue_.push_back(std::move(value));
    not_empty_.notify_one();
    return ok();
  }

  [[nodiscard]] Result<T> pop(const CancellationToken& token = CancellationToken{}) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this, &token] {
      return !queue_.empty() || (closed_ && queue_.empty()) || aborted_ || token.cancelled();
    });
    if (!queue_.empty()) {
      T value = std::move(queue_.front());
      queue_.pop_front();
      not_full_.notify_one();
      return value;
    }
    if (aborted_) {
      return make_error(ErrorCode::Cancelled, "queue aborted");
    }
    if (token.cancelled()) {
      return make_error(ErrorCode::Cancelled, "cancelled while waiting for work");
    }
    return make_error(ErrorCode::QueueClosed, "queue closed and drained");
  }

  [[nodiscard]] std::optional<T> try_pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return value;
  }

  // Refuses further pushes but lets consumers drain what is already queued.
  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  // Drops all pending work and releases every waiter. Used by hard shutdown.
  void abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    queue_.clear();
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }
  [[nodiscard]] bool aborted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> queue_;
  std::size_t capacity_;
  bool closed_ = false;
  bool aborted_ = false;
};

// ---------------------------------------------------------------------------
// Lock order and reentrancy audit
//
// The ledger has a small number of mutexes with a documented rank. Every
// acquisition is checked: a thread that re-enters a mutex it already holds, or
// that acquires a lower ranked mutex while holding a higher ranked one, is
// detected, counted, and reported to the caller instead of deadlocking.
// ---------------------------------------------------------------------------
struct LockAuditCounters {
  std::atomic<std::uint64_t> acquisitions{0};
  std::atomic<std::uint64_t> reentrancy_detections{0};
  std::atomic<std::uint64_t> lock_order_violations{0};
};

struct LockAuditCountersSnapshot {
  std::uint64_t acquisitions = 0;
  std::uint64_t reentrancy_detections = 0;
  std::uint64_t lock_order_violations = 0;
};

[[nodiscard]] LockAuditCounters& lock_audit() noexcept;
[[nodiscard]] LockAuditCountersSnapshot lock_audit_snapshot() noexcept;
void reset_lock_audit() noexcept;

// Documented ranks. Lower rank means "outer". Acquiring a mutex out of rank
// order is a defect and is reported.
enum class LockRank : std::uint32_t {
  Registry = 10,
  StoreState = 20,
  Segment = 30,
  Index = 40,
  Diagnostics = 50,
};

[[nodiscard]] std::string_view lock_rank_name(LockRank rank) noexcept;

class CheckedMutex {
 public:
  class Guard {
   public:
    Guard() = default;
    Guard(CheckedMutex* owner, bool reentrant, bool order_violation, const char* site)
        : owner_(owner), reentrant_(reentrant), order_violation_(order_violation), site_(site) {}
    ~Guard() { release(); }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept { move_from(other); }
    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        release();
        move_from(other);
      }
      return *this;
    }

    [[nodiscard]] bool owns_lock() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] bool reentrant() const noexcept { return reentrant_; }
    [[nodiscard]] bool order_violation() const noexcept { return order_violation_; }
    [[nodiscard]] const char* site() const noexcept { return site_; }
    void release() noexcept;

   private:
    void move_from(Guard& other) noexcept {
      owner_ = other.owner_;
      reentrant_ = other.reentrant_;
      order_violation_ = other.order_violation_;
      site_ = other.site_;
      other.owner_ = nullptr;
      other.reentrant_ = false;
      other.order_violation_ = false;
      other.site_ = nullptr;
    }

    CheckedMutex* owner_ = nullptr;
    bool reentrant_ = false;
    bool order_violation_ = false;
    const char* site_ = nullptr;
  };

  explicit CheckedMutex(LockRank rank = LockRank::StoreState) : rank_(rank) {}
  CheckedMutex(const CheckedMutex&) = delete;
  CheckedMutex& operator=(const CheckedMutex&) = delete;
  ~CheckedMutex() = default;

  [[nodiscard]] Guard acquire(const char* site);

  [[nodiscard]] LockRank rank() const noexcept { return rank_; }
  [[nodiscard]] bool held_by_current_thread() const noexcept;

  [[nodiscard]] std::mutex& native() noexcept { return mutex_; }

 private:
  friend class Guard;

  std::mutex mutex_;
  LockRank rank_;
};

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------
enum class ShutdownMode {
  Drain = 1,  // finish queued work, then join
  Abort = 2,  // cancel immediately, drop pending work, then join
};

struct PoolStats {
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t rejected = 0;
  std::uint64_t peak_queue = 0;
  std::uint64_t jobs_in_flight = 0;
  bool running = false;
};

struct PoolConfig {
  std::uint32_t workers = 4;
  std::size_t queue_depth = 4096;
  std::string name = "fel-pool";
};

// Returns the number of usable hardware threads, capped by limits::kMaxWorkers.
[[nodiscard]] std::uint32_t hardware_worker_budget() noexcept;

class ThreadPool {
 public:
  using Job = std::function<void(const CancellationToken&)>;

  explicit ThreadPool(PoolConfig config = {});
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  [[nodiscard]] Result<void> start();
  [[nodiscard]] Result<void> submit(Job job);
  void shutdown(ShutdownMode mode);
  void cancel() noexcept { cancel_.cancel(); }

  [[nodiscard]] PoolStats stats() const;
  [[nodiscard]] const CancellationToken& token() const noexcept { return cancel_.token(); }
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::size_t queue_depth() const noexcept { return queue_.capacity(); }
  [[nodiscard]] const std::string& name() const noexcept { return config_.name; }

 private:
  void worker_loop();

  PoolConfig config_;
  BoundedQueue<Job> queue_;
  CancellationSource cancel_;
  std::vector<std::thread> threads_;
  mutable std::mutex stats_mutex_;
  PoolStats stats_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> in_flight_{0};
  std::atomic<std::uint64_t> peak_queue_{0};
  std::mutex lifecycle_mutex_;
};

// Deterministic, seedable pseudo random generator used by property tests and by
// seeded randomized workloads. Not used on any accounting path.
class XorShift64 {
 public:
  explicit XorShift64(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t x = state_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state_ = x;
    return x;
  }
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next() % bound;
  }

 private:
  std::uint64_t state_;
};

}  // namespace fel
