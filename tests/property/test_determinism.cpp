// Fabric Efficiency Ledger - identical evidence and policy give identical totals.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] std::vector<Observation> evidence_set(const feltest::Fixture& fixture,
                                                    std::size_t count) {
  XorShift64 rng(0x5EEDULL);
  std::vector<Observation> observations;
  for (std::size_t i = 0; i < count; ++i) {
    const bool synthetic = rng.below(4) == 0;
    auto spec = feltest::make_spec(fixture,
                                   synthetic ? fixture.probe_source : fixture.counter_source,
                                   synthetic ? fixture.probe_incarnation
                                             : fixture.counter_incarnation,
                                   i + 1, rng.below(5000) + 1,
                                   static_cast<Category>(1 + rng.below(kCategoryCount)),
                                   rng.below(2) == 0 ? fixture.port1 : fixture.port2);
    const auto offset = Duration::from_minutes(static_cast<std::int64_t>(rng.below(50)));
    spec.window_start = fixture.period_start + offset;
    spec.window_end = spec.window_start + Duration::from_minutes(1);
    spec.observed_at = spec.window_end;
    spec.received_at = spec.window_end;
    observations.push_back(feltest::make_observation(spec));
  }
  return observations;
}

struct Digest {
  std::string content;
  std::string summary;
};

[[nodiscard]] Result<Digest> run(const std::vector<Observation>& observations,
                                 const feltest::Fixture& fixture, bool parallel) {
  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  if (!ledger.has_value()) {
    return ledger.error();
  }
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  if (!period.has_value()) {
    return period.error();
  }
  const auto batch = feltest::make_batch(observations);
  auto ingest_report = parallel ? ledger.value()->ingest_parallel(batch)
                                : ledger.value()->ingest(batch);
  if (!ingest_report.has_value()) {
    return ingest_report.error();
  }
  auto closed = ledger.value()->close_period(
      CloseRequest{period.value(), fixture.period_end, "close", "test", false});
  if (!closed.has_value()) {
    return closed.error();
  }
  QueryRequest request;
  request.period = period.value();
  auto summary = ledger.value()->query(request);
  if (!summary.has_value()) {
    return summary.error();
  }
  Digest digest;
  digest.content = closed.value().content_digest.to_hex();
  digest.summary = summary.value().canonical_form();
  return digest;
}

}  // namespace

FEL_TEST(property, ingest_order_does_not_change_a_single_total) {
  const auto fixture = feltest::make_fixture();
  const auto observations = evidence_set(fixture, 200);

  auto baseline = run(observations, fixture, false);
  FEL_REQUIRE(baseline.has_value());

  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    XorShift64 rng(seed);
    auto shuffled = observations;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rng.below(i));
      std::swap(shuffled[i - 1], shuffled[j]);
    }
    auto permuted = run(shuffled, fixture, false);
    FEL_REQUIRE(permuted.has_value());
    FEL_EXPECT_EQ(permuted.value().content, baseline.value().content);
    FEL_EXPECT_EQ(permuted.value().summary, baseline.value().summary);
  }
}

FEL_TEST(property, repeated_runs_are_byte_identical) {
  const auto fixture = feltest::make_fixture();
  const auto observations = evidence_set(fixture, 150);
  auto first = run(observations, fixture, false);
  FEL_REQUIRE(first.has_value());
  for (int i = 0; i < 3; ++i) {
    auto again = run(observations, fixture, false);
    FEL_REQUIRE(again.has_value());
    FEL_EXPECT_EQ(again.value().content, first.value().content);
    FEL_EXPECT_EQ(again.value().summary, first.value().summary);
  }
}

FEL_TEST(property, parallel_ingest_matches_sequential_ingest) {
  const auto fixture = feltest::make_fixture();
  const auto observations = evidence_set(fixture, 300);
  auto sequential = run(observations, fixture, false);
  auto parallel = run(observations, fixture, true);
  FEL_EXPECT(sequential.has_value());
  FEL_EXPECT(parallel.has_value());
  if (sequential.has_value() && parallel.has_value()) {
    FEL_EXPECT_EQ(parallel.value().content, sequential.value().content);
    FEL_EXPECT_EQ(parallel.value().summary, sequential.value().summary);
  }
}

FEL_TEST(property, aggregation_is_deterministic_across_repeats) {
  const auto fixture = feltest::make_fixture();
  const auto observations = evidence_set(fixture, 120);
  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());
  FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch(observations)));
  FEL_ASSERT_OK(ledger.value()->close_period(
      CloseRequest{period.value(), fixture.period_end, "close", "test", false}));

  for (const auto axis : {AggregateAxis::Category, AggregateAxis::Scope,
                          AggregateAxis::Generation, AggregateAxis::Source,
                          AggregateAxis::Provenance, AggregateAxis::Resource}) {
    AggregateRequest request;
    request.period = period.value();
    request.axis = axis;
    auto first = ledger.value()->aggregate(request);
    auto second = ledger.value()->aggregate(request);
    FEL_REQUIRE(first.has_value());
    FEL_REQUIRE(second.has_value());
    FEL_EXPECT_EQ(first.value().canonical_form(), second.value().canonical_form());
    FEL_EXPECT_EQ(first.value().digest.to_hex(), second.value().digest.to_hex());
    FEL_EXPECT_EQ(first.value().to_json(), second.value().to_json());
  }
}
