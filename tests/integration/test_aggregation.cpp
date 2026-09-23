// Fabric Efficiency Ledger - bounded hierarchical and time aggregation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] std::uint64_t bucket_total(const AggregationResult& result, const std::string& label) {
  for (const auto& bucket : result.buckets) {
    if (bucket.key.label() == label) {
      std::uint64_t total = 0;
      for (const auto& cell : bucket.cells) {
        total += cell.known_total;
      }
      return total;
    }
  }
  return 0;
}

}  // namespace

FEL_TEST(integration, category_axis_partitions_the_attributed_total) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  std::vector<Observation> observations;
  observations.push_back(feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 1000,
      Category::UsefulDeliveredWork, rig.fixture.port1)));
  observations.push_back(feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 2, 250,
      Category::Retransmission, rig.fixture.port1)));
  observations.push_back(feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 3, 50,
      Category::ControlOverhead, rig.fixture.port1)));
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(observations)));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  AggregateRequest request;
  request.period = rig.period;
  request.axis = AggregateAxis::Category;
  auto result = rig.ledger->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(bucket_total(result.value(), "useful-delivered-work"), std::uint64_t{1000});
  FEL_EXPECT_EQ(bucket_total(result.value(), "retransmission"), std::uint64_t{250});
  FEL_EXPECT_EQ(bucket_total(result.value(), "control-overhead"), std::uint64_t{50});
}

FEL_TEST(integration, scope_rollup_sums_children_without_double_counting) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  std::vector<Observation> observations;
  observations.push_back(feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 400,
      Category::UsefulDeliveredWork, rig.fixture.port1)));
  observations.push_back(feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 2, 600,
      Category::UsefulDeliveredWork, rig.fixture.port2)));
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(observations)));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  QueryRequest request;
  request.period = rig.period;
  auto summary = rig.ledger->query(request);
  FEL_REQUIRE(summary.has_value());

  std::uint64_t leaf_total = 0;
  for (const auto& row : summary.value().rows) {
    leaf_total += row.cell.known_total;
  }
  FEL_EXPECT_EQ(leaf_total, std::uint64_t{1000});

  std::uint64_t root_total = 0;
  std::uint64_t switch_total = 0;
  for (const auto& row : summary.value().rolled_up) {
    if (row.scope == rig.fixture.fabric_scope) {
      root_total += row.cell.known_total;
    }
    if (row.scope == rig.fixture.switch_scope) {
      switch_total += row.cell.known_total;
    }
  }
  // A rollup never exceeds the total of the disjoint leaves below it.
  FEL_EXPECT_EQ(root_total, std::uint64_t{1000});
  FEL_EXPECT_EQ(switch_total, std::uint64_t{1000});
}

FEL_TEST(integration, time_axis_buckets_by_window_completion) {
  // A three hour period so that two different hourly buckets are available.
  const auto fixture = feltest::make_fixture();
  const TimePoint period_end = fixture.period_start + Duration::from_hours(3);
  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, period_end, "p");
  FEL_REQUIRE(period.has_value());

  auto first = feltest::make_spec(fixture, fixture.counter_source, fixture.counter_incarnation, 1,
                                  100, Category::UsefulDeliveredWork, fixture.port1);
  first.window_start = fixture.period_start;
  first.window_end = fixture.period_start + Duration::from_minutes(10);
  first.observed_at = first.window_end;
  first.received_at = first.window_end;

  auto second = feltest::make_spec(fixture, fixture.counter_source, fixture.counter_incarnation, 2,
                                   300, Category::UsefulDeliveredWork, fixture.port1);
  second.window_start = fixture.period_start + Duration::from_minutes(65);
  second.window_end = fixture.period_start + Duration::from_minutes(75);
  second.observed_at = second.window_end;
  second.received_at = second.window_end;

  FEL_ASSERT_OK(ledger.value()->ingest(
      feltest::make_batch({feltest::make_observation(first), feltest::make_observation(second)})));
  const PeriodId period_id = period.value();
  FEL_ASSERT_OK(ledger.value()->close_period(
      CloseRequest{period_id, period_end, "close", "test", false}));

  AggregateRequest request;
  request.period = period_id;
  request.axis = AggregateAxis::Time;
  request.window = Duration::from_hours(1);
  auto result = ledger.value()->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(result.value().buckets.size(), std::size_t{2});
  FEL_EXPECT_EQ(result.value().buckets[0].key.bucket_start, fixture.period_start);
  FEL_EXPECT_EQ(result.value().buckets[1].key.bucket_start,
                fixture.period_start + Duration::from_hours(1));
  std::uint64_t total = 0;
  for (const auto& bucket : result.value().buckets) {
    FEL_REQUIRE(bucket.key.bucket_end > bucket.key.bucket_start);
    for (const auto& cell : bucket.cells) {
      total += cell.known_total;
    }
  }
  FEL_EXPECT_EQ(total, std::uint64_t{400});
}

FEL_TEST(integration, generated_axis_partitions_by_generation) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  const auto observation = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 700,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({observation})));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  AggregateRequest request;
  request.period = rig.period;
  request.axis = AggregateAxis::Generation;
  auto result = rig.ledger->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(result.value().buckets.size(), std::size_t{1});
  FEL_EXPECT_EQ(result.value().buckets.front().key.generation, rig.fixture.generation);
}

FEL_TEST(integration, provenance_axis_labels_real_and_synthetic) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto real = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  auto synthetic = feltest::make_spec(rig.fixture, rig.fixture.probe_source,
                                      rig.fixture.probe_incarnation, 2, 200,
                                      Category::UsefulDeliveredWork, rig.fixture.port2);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(
      {feltest::make_observation(real), feltest::make_observation(synthetic)})));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  AggregateRequest request;
  request.period = rig.period;
  request.axis = AggregateAxis::Provenance;
  auto result = rig.ledger->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(bucket_total(result.value(), "real"), std::uint64_t{100});
  FEL_EXPECT_EQ(bucket_total(result.value(), "synthetic"), std::uint64_t{200});
}

FEL_TEST(integration, aggregation_respects_the_bucket_budget) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  std::vector<Observation> observations;
  for (std::uint64_t i = 0; i < 4; ++i) {
    observations.push_back(feltest::make_observation(feltest::make_spec(
        rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 10 + i, 10,
        static_cast<Category>(1 + (i % kCategoryCount)), rig.fixture.port1)));
  }
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(observations)));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  AggregateRequest request;
  request.period = rig.period;
  request.axis = AggregateAxis::Category;
  request.max_buckets = 2;
  auto result = rig.ledger->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(result.value().buckets.size(), std::size_t{2});
  FEL_EXPECT(result.value().truncation.truncated);
  FEL_EXPECT(result.value().truncation.total_available > 2);
}

FEL_TEST(integration, aggregation_rejects_a_window_outside_the_permitted_range) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  AggregateRequest request;
  request.period = rig.period;
  request.axis = AggregateAxis::Time;
  request.window = Duration::from_nanos(1);
  FEL_ASSERT_ERR(rig.ledger->aggregate(request), ErrorCode::InvalidArgument);
}

FEL_TEST(integration, time_axis_is_unavailable_after_the_evidence_is_folded) {
  feltest::ScopedTempDir directory("aggregate-fold");
  auto fixture = feltest::make_fixture();
  OpenOptions options;
  options.store_path = directory.child("store");
  options.mode = StoreOpenMode::OpenOrCreate;
  options.policy = feltest::test_policy();
  options.topology = fixture.topology;
  options.as_of = fixture.period_end;
  auto ledger = Ledger::open(options);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());
  const auto observation = feltest::make_observation(feltest::make_spec(
      fixture, fixture.counter_source, fixture.counter_incarnation, 1, 500,
      Category::UsefulDeliveredWork, fixture.port1));
  FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({observation})));
  FEL_ASSERT_OK(ledger.value()->close_period(
      CloseRequest{period.value(), fixture.period_end, "close", "test", false}));
  auto compaction = ledger.value()->compact_closed_periods(false);
  FEL_REQUIRE(compaction.has_value());
  FEL_EXPECT(compaction.value().performed);

  AggregateRequest request;
  request.period = period.value();
  request.axis = AggregateAxis::Time;
  request.window = Duration::from_hours(1);
  FEL_ASSERT_ERR(ledger.value()->aggregate(request), ErrorCode::UnsupportedQuery);

  // The category axis is served from the immutable revision and still works.
  request.axis = AggregateAxis::Category;
  request.window.reset();
  auto result = ledger.value()->aggregate(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(bucket_total(result.value(), "useful-delivered-work"), std::uint64_t{500});
}
