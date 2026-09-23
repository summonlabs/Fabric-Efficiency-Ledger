// Fabric Efficiency Ledger - concurrency, races and cancellation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] std::vector<Observation> make_evidence(const feltest::Fixture& fixture,
                                                     std::size_t count) {
  std::vector<Observation> observations;
  observations.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const bool synthetic = (i % 5) == 0;
    auto spec = feltest::make_spec(fixture,
                                   synthetic ? fixture.probe_source : fixture.counter_source,
                                   synthetic ? fixture.probe_incarnation
                                             : fixture.counter_incarnation,
                                   i + 1, 10 + (i % 97),
                                   static_cast<Category>(1 + (i % kCategoryCount)),
                                   (i % 2) == 0 ? fixture.port1 : fixture.port2);
    // The fixture period is one hour wide; every window must fall inside it.
    const auto offset = Duration::from_minutes(static_cast<std::int64_t>(i % 50));
    spec.window_start = fixture.period_start + offset;
    spec.window_end = spec.window_start + Duration::from_minutes(1);
    spec.observed_at = spec.window_end;
    spec.received_at = spec.window_end;
    observations.push_back(feltest::make_observation(spec));
  }
  return observations;
}

[[nodiscard]] std::string close_and_digest(Ledger& ledger, const PeriodId& period,
                                           TimePoint closed_at) {
  auto closed = ledger.close_period(CloseRequest{period, closed_at, "close", "test", false});
  if (!closed.has_value()) {
    return "error:" + closed.error().to_string();
  }
  return closed.value().content_digest.to_hex();
}

}  // namespace

FEL_TEST(concurrency, concurrent_ingest_of_disjoint_shards_matches_sequential) {
  const auto fixture = feltest::make_fixture();
  const auto evidence = make_evidence(fixture, 480);

  auto sequential = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(sequential.has_value());
  auto sequential_period =
      sequential.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(sequential_period.has_value());
  FEL_ASSERT_OK(sequential.value()->ingest(feltest::make_batch(evidence)));
  const std::string expected =
      close_and_digest(*sequential.value(), sequential_period.value(), fixture.period_end);

  auto concurrent = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(concurrent.has_value());
  auto concurrent_period =
      concurrent.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(concurrent_period.has_value());

  constexpr std::size_t kThreads = 6;
  std::vector<std::thread> threads;
  std::atomic<std::size_t> failures{0};
  threads.reserve(kThreads);
  for (std::size_t worker = 0; worker < kThreads; ++worker) {
    threads.emplace_back([&, worker]() {
      std::vector<Observation> shard;
      for (std::size_t i = worker; i < evidence.size(); i += kThreads) {
        shard.push_back(evidence[i]);
      }
      // Interleave the shards so that the store sees a genuinely interleaved
      // arrival order rather than one contiguous run per thread.
      for (std::size_t round = 0; round < 3; ++round) {
        auto report = concurrent.value()->ingest(feltest::make_batch(shard));
        if (!report.has_value() &&
            report.error().code != ErrorCode::QueueClosed) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  FEL_EXPECT_EQ(failures.load(std::memory_order_relaxed), std::size_t{0});
  FEL_EXPECT_EQ(concurrent.value()->retained_observation_count(),
                static_cast<std::uint64_t>(evidence.size()));
  const std::string actual =
      close_and_digest(*concurrent.value(), concurrent_period.value(), fixture.period_end);
  FEL_EXPECT_EQ(actual, expected);
}

FEL_TEST(concurrency, concurrent_readers_see_a_consistent_period) {
  const auto fixture = feltest::make_fixture();
  const auto evidence = make_evidence(fixture, 240);
  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());
  FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch(evidence)));
  FEL_ASSERT_OK(ledger.value()->close_period(
      CloseRequest{period.value(), fixture.period_end, "close", "test", false}));

  std::atomic<std::size_t> mismatches{0};
  std::vector<std::thread> threads;
  for (std::size_t worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&]() {
      QueryRequest request;
      request.period = period.value();
      for (int i = 0; i < 25; ++i) {
        auto summary = ledger.value()->query(request);
        if (!summary.has_value()) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        std::uint64_t total = 0;
        for (const auto& row : summary.value().rows) {
          total += row.cell.known_total;
        }
        std::uint64_t expected = 0;
        for (const auto& observation : evidence) {
          expected += observation.amount;
        }
        if (total != expected) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  FEL_EXPECT_EQ(mismatches.load(std::memory_order_relaxed), std::size_t{0});
}

FEL_TEST(concurrency, parallel_ingest_uses_the_worker_pool_and_agrees_with_sequential) {
  const auto fixture = feltest::make_fixture();
  const auto evidence = make_evidence(fixture, 400);

  auto policy = feltest::test_policy();
  policy.workers = 4;
  policy.queue_depth = 64;

  auto ledger = Ledger::in_memory(policy, fixture.topology, fixture.period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());
  auto report = ledger.value()->ingest_parallel(feltest::make_batch(evidence));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().accepted, static_cast<std::uint64_t>(evidence.size()));

  auto reference = Ledger::in_memory(policy, fixture.topology, fixture.period_end);
  FEL_REQUIRE(reference.has_value());
  auto reference_period =
      reference.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(reference_period.has_value());
  FEL_ASSERT_OK(reference.value()->ingest(feltest::make_batch(evidence)));

  FEL_EXPECT_EQ(close_and_digest(*ledger.value(), period.value(), fixture.period_end),
                close_and_digest(*reference.value(), reference_period.value(),
                                 fixture.period_end));
}

FEL_TEST(concurrency, lock_audit_reports_no_reentrancy_or_order_violation_under_load) {
  reset_lock_audit();
  const auto fixture = feltest::make_fixture();
  const auto evidence = make_evidence(fixture, 160);
  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());

  std::vector<std::thread> threads;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&]() {
      QueryRequest request;
      request.period = period.value();
      for (int i = 0; i < 20; ++i) {
        (void)ledger.value()->stats();
        (void)ledger.value()->query(request);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto snapshot = lock_audit_snapshot();
  FEL_EXPECT_EQ(snapshot.reentrancy_detections, std::uint64_t{0});
  FEL_EXPECT_EQ(snapshot.lock_order_violations, std::uint64_t{0});
  FEL_EXPECT(snapshot.acquisitions > 0);
}

FEL_TEST(concurrency, worker_pool_cancellation_is_real) {
  CancellationSource source;
  BoundedQueue<int> queue(4);
  std::atomic<int> processed{0};
  std::atomic<bool> stop{false};

  std::thread producer([&]() {
    for (int i = 0; i < 1000 && !stop.load(std::memory_order_relaxed); ++i) {
      auto pushed = queue.try_push(i);
      if (!pushed.has_value()) {
        break;
      }
    }
    queue.close();
  });

  std::thread consumer([&]() {
    while (true) {
      auto item = queue.pop(source.token());
      if (!item.has_value()) {
        break;
      }
      processed.fetch_add(1, std::memory_order_relaxed);
    }
  });

  source.cancel();
  queue.abort();
  producer.join();
  consumer.join();
  stop.store(true, std::memory_order_relaxed);
  FEL_EXPECT(processed.load(std::memory_order_relaxed) <= 1000);
}

FEL_TEST(concurrency, thread_pool_shutdown_joins_every_worker) {
  PoolConfig config;
  config.workers = 4;
  config.queue_depth = 128;
  config.name = "test-pool";
  ThreadPool pool(config);
  FEL_ASSERT_OK(pool.start());
  std::atomic<std::uint64_t> counter{0};
  for (int i = 0; i < 5000; ++i) {
    auto submitted = pool.submit([&counter](const CancellationToken&) {
      counter.fetch_add(1, std::memory_order_relaxed);
    });
    if (!submitted.has_value()) {
      break;
    }
  }
  pool.shutdown(ShutdownMode::Drain);
  FEL_EXPECT_EQ(counter.load(std::memory_order_relaxed), pool.stats().completed);
  FEL_EXPECT_EQ(pool.stats().jobs_in_flight, std::uint64_t{0});
  FEL_EXPECT(!pool.running());
}
