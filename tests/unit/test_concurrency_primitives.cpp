// Fabric Efficiency Ledger - bounded queue, thread pool, lock audit.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <thread>
#include <vector>

#include "fel/concurrency.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, bounded_queue_enforces_capacity_and_fifo_order) {
  BoundedQueue<int> queue(4);
  FEL_EXPECT_EQ(queue.capacity(), std::size_t{4});
  for (int i = 0; i < 4; ++i) {
    FEL_ASSERT_OK(queue.push(i));
  }
  FEL_EXPECT_EQ(queue.size(), std::size_t{4});
  FEL_ASSERT_ERR(queue.try_push(99), ErrorCode::QueueFull);
  for (int i = 0; i < 4; ++i) {
    auto item = queue.pop();
    FEL_REQUIRE(item.has_value());
    FEL_EXPECT_EQ(item.value(), i);
  }
  FEL_EXPECT_EQ(queue.size(), std::size_t{0});
}

FEL_TEST(unit, bounded_queue_close_drains_then_reports_closed) {
  BoundedQueue<int> queue(2);
  FEL_ASSERT_OK(queue.push(7));
  queue.close();
  auto drained = queue.pop();
  FEL_REQUIRE(drained.has_value());
  FEL_EXPECT_EQ(drained.value(), 7);
  FEL_ASSERT_ERR(queue.pop(), ErrorCode::QueueClosed);
  FEL_ASSERT_ERR(queue.push(1), ErrorCode::QueueClosed);
}

FEL_TEST(unit, bounded_queue_abort_drops_pending_work) {
  BoundedQueue<int> queue(8);
  for (int i = 0; i < 5; ++i) {
    FEL_ASSERT_OK(queue.try_push(i));
  }
  queue.abort();
  FEL_EXPECT_EQ(queue.size(), std::size_t{0});
  FEL_ASSERT_ERR(queue.pop(), ErrorCode::Cancelled);
}

FEL_TEST(unit, thread_pool_runs_every_submitted_job) {
  PoolConfig config;
  config.workers = 4;
  config.queue_depth = 256;
  ThreadPool pool(config);
  FEL_ASSERT_OK(pool.start());
  FEL_ASSERT_ERR(pool.start(), ErrorCode::InvalidArgument);

  std::atomic<std::uint64_t> counter{0};
  constexpr int kJobs = 5000;
  std::uint64_t accepted = 0;
  for (int i = 0; i < kJobs; ++i) {
    auto submitted = pool.submit([&counter](const CancellationToken&) {
      counter.fetch_add(1, std::memory_order_relaxed);
    });
    if (!submitted.has_value()) {
      // The queue is bounded on purpose: a producer that outruns the consumers
      // is refused rather than allowed to grow without limit.
      FEL_EXPECT_EQ(submitted.error().code, ErrorCode::QueueFull);
      break;
    }
    ++accepted;
  }
  FEL_EXPECT(accepted > 0);
  pool.shutdown(ShutdownMode::Drain);
  FEL_EXPECT_EQ(counter.load(std::memory_order_relaxed), accepted);
  const PoolStats stats = pool.stats();
  FEL_EXPECT_EQ(stats.completed, accepted);
  FEL_EXPECT_EQ(stats.jobs_in_flight, std::uint64_t{0});
  FEL_EXPECT(!pool.running());
  FEL_ASSERT_ERR(pool.submit([](const CancellationToken&) {}), ErrorCode::QueueClosed);
}

FEL_TEST(unit, thread_pool_abort_is_real_cancellation) {
  PoolConfig config;
  config.workers = 4;
  config.queue_depth = 32;
  ThreadPool pool(config);
  FEL_ASSERT_OK(pool.start());

  std::atomic<std::uint64_t> entered{0};
  std::atomic<std::uint64_t> observed_cancel{0};
  for (int i = 0; i < 256; ++i) {
    (void)pool.submit([&entered, &observed_cancel](const CancellationToken& token) {
      entered.fetch_add(1, std::memory_order_relaxed);
      while (!token.cancelled()) {
        std::this_thread::yield();
      }
      observed_cancel.fetch_add(1, std::memory_order_relaxed);
    });
  }
  pool.cancel();
  pool.shutdown(ShutdownMode::Abort);
  FEL_EXPECT(!pool.running());
  FEL_EXPECT_EQ(pool.stats().jobs_in_flight, std::uint64_t{0});
  FEL_EXPECT(entered.load(std::memory_order_relaxed) <= 256);
}

FEL_TEST(unit, lock_audit_detects_out_of_order_acquisition) {
  reset_lock_audit();
  CheckedMutex store_state(LockRank::StoreState);
  CheckedMutex registry(LockRank::Registry);
  {
    auto outer = store_state.acquire("test.outer");
    FEL_EXPECT(outer.owns_lock());
    FEL_EXPECT(!outer.order_violation());
    auto inner = registry.acquire("test.inner");
    FEL_EXPECT(inner.owns_lock());
    FEL_EXPECT(inner.order_violation());
  }
  const auto snapshot = lock_audit_snapshot();
  FEL_EXPECT_EQ(snapshot.lock_order_violations, std::uint64_t{1});
  FEL_EXPECT_EQ(snapshot.reentrancy_detections, std::uint64_t{0});
  FEL_EXPECT(snapshot.acquisitions >= 2);
  reset_lock_audit();
  FEL_EXPECT_EQ(lock_audit_snapshot().acquisitions, std::uint64_t{0});
}

FEL_TEST(unit, checked_mutex_guard_move_and_release_semantics) {
  CheckedMutex mutex(LockRank::Index);
  {
    auto guard = mutex.acquire("test.guard");
    FEL_EXPECT(guard.owns_lock());
    FEL_EXPECT(mutex.held_by_current_thread());
    auto moved = std::move(guard);
    FEL_EXPECT(!guard.owns_lock());
    FEL_EXPECT(moved.owns_lock());
    moved.release();
    FEL_EXPECT(!moved.owns_lock());
    moved.release();  // idempotent
    FEL_EXPECT(!mutex.held_by_current_thread());
  }
  FEL_EXPECT(!mutex.held_by_current_thread());
  FEL_EXPECT(!lock_rank_name(LockRank::Segment).empty());
}

FEL_TEST(unit, worker_budget_is_bounded) {
  const std::uint32_t budget = hardware_worker_budget();
  FEL_EXPECT(budget >= 1);
  FEL_EXPECT(budget <= limits::kMaxWorkers);
}

FEL_TEST(unit, xor_shift_is_deterministic_and_bounded) {
  XorShift64 first(12345);
  XorShift64 second(12345);
  for (int i = 0; i < 100; ++i) {
    FEL_EXPECT_EQ(first.next(), second.next());
  }
  XorShift64 bounded(999);
  for (int i = 0; i < 100; ++i) {
    FEL_EXPECT(bounded.below(7) < 7);
  }
  FEL_EXPECT_EQ(bounded.below(0), std::uint64_t{0});
}
