// Fabric Efficiency Ledger - ingest, fencing and period lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] std::size_t claim_total(const PeriodRevision& revision) {
  std::uint64_t total = 0;
  for (const auto& claim : revision.claims) {
    total += claim.cell.known_total;
  }
  return static_cast<std::size_t>(total);
}

}  // namespace

FEL_TEST(integration, duplicate_delivery_is_counted_once) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 7, 4000,
                                       Category::UsefulDeliveredWork, rig.fixture.port1);
  const auto observation = feltest::make_observation(spec);
  // The same physical measurement re-delivered through a second ingest call.
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({observation})));
  auto second = rig.ledger->ingest(feltest::make_batch({observation}));
  FEL_REQUIRE(second.has_value());
  FEL_EXPECT_EQ(second.value().duplicate_suppressed, std::uint64_t{1});
  FEL_EXPECT_EQ(second.value().accepted, std::uint64_t{0});

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(claim_total(closed.value()), std::size_t{4000});
}

FEL_TEST(integration, mirrored_origin_through_a_second_source_is_counted_once) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto first = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                  rig.fixture.counter_incarnation, 11, 1500,
                                  Category::UsefulDeliveredWork, rig.fixture.port1);
  auto mirrored = feltest::make_spec(rig.fixture, rig.fixture.probe_source,
                                     rig.fixture.probe_incarnation, 3, 1500,
                                     Category::UsefulDeliveredWork, rig.fixture.port1);
  // Same reporting window and the same amount, delivered by two independent
  // agents: the cell must be counted once, and the suppression explained.
  mirrored.window_start = first.window_start;
  mirrored.window_end = first.window_end;

  FEL_ASSERT_OK(rig.ledger->ingest(
      feltest::make_batch({feltest::make_observation(first), feltest::make_observation(mirrored)})));
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(claim_total(closed.value()), std::size_t{1500});
  FEL_EXPECT(closed.value().duplicate_suppressed >= 1);
}

FEL_TEST(integration, stale_generation_never_attaches_to_current_resources) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 21, 5000,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  // The resource is only bound to the current generation, so evidence dated to
  // the superseded generation must not attach to it.
  spec.generation = rig.fixture.superseded_generation;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(closed.value().claims.size(), std::size_t{0});
  bool saw_stale_generation = false;
  for (const auto& unknown : closed.value().unknowns) {
    if (unknown.reason == UnknownReason::StaleGeneration) {
      saw_stale_generation = true;
    }
  }
  FEL_EXPECT(saw_stale_generation);
}

FEL_TEST(integration, evidence_without_a_period_is_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 31, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  spec.window_start = rig.fixture.period_end + Duration::from_hours(1);
  spec.window_end = rig.fixture.period_end + Duration::from_hours(2);
  spec.observed_at = spec.window_start;
  spec.received_at = spec.window_start;

  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_no_period, std::uint64_t{1});
  FEL_EXPECT_EQ(report.value().accepted, std::uint64_t{0});
}

FEL_TEST(integration, window_spanning_two_periods_is_refused_rather_than_split) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  // A second period that follows the first without overlapping it.
  const auto second_start = rig.fixture.period_end;
  FEL_ASSERT_OK(
      rig.ledger->open_period(second_start, second_start + Duration::from_hours(1), "period-2"));

  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 41, 900,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  // Straddles the boundary: contained in neither period, so it cannot be
  // counted in either without double counting.
  spec.window_start = rig.fixture.period_end - Duration::from_minutes(10);
  spec.window_end = rig.fixture.period_end + Duration::from_minutes(10);
  spec.observed_at = spec.window_end;
  spec.received_at = spec.window_end;

  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_no_period, std::uint64_t{1});
}

FEL_TEST(integration, closed_period_refuses_late_evidence_under_the_reject_policy) {
  auto policy = feltest::test_policy();
  policy.late_evidence = LateEvidencePolicy::Reject;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  const auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 51, 700,
                                       Category::UsefulDeliveredWork, rig.fixture.port1);
  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().late_evidence, std::uint64_t{1});
  FEL_EXPECT_EQ(report.value().accepted, std::uint64_t{0});
}

FEL_TEST(integration, period_cannot_be_closed_twice) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  FEL_ASSERT_ERR(rig.ledger->close_period(
                     CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}),
                 ErrorCode::PeriodAlreadyClosed);
}

FEL_TEST(integration, overlapping_periods_are_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_ERR(rig.ledger->open_period(rig.fixture.period_start + Duration::from_minutes(1),
                                         rig.fixture.period_end + Duration::from_minutes(1),
                                         "overlap"),
                 ErrorCode::DuplicateDefinition);
}

FEL_TEST(integration, retained_evidence_budget_is_enforced) {
  auto policy = feltest::test_policy();
  policy.max_retained_observations = 2;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  std::vector<Observation> observations;
  for (std::uint64_t i = 0; i < 3; ++i) {
    observations.push_back(feltest::make_observation(feltest::make_spec(
        rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 100 + i, 10,
        Category::UsefulDeliveredWork, rig.fixture.port1)));
  }
  FEL_ASSERT_ERR(rig.ledger->ingest(feltest::make_batch(observations)),
                 ErrorCode::CapacityExceeded);
  FEL_EXPECT_EQ(rig.ledger->retained_observation_count(), std::uint64_t{0});
}

FEL_TEST(integration, batch_larger_than_the_policy_budget_is_refused) {
  auto policy = feltest::test_policy();
  policy.max_batch_records = 2;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  std::vector<Observation> observations;
  for (std::uint64_t i = 0; i < 3; ++i) {
    observations.push_back(feltest::make_observation(feltest::make_spec(
        rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 200 + i, 10,
        Category::UsefulDeliveredWork, rig.fixture.port1)));
  }
  FEL_ASSERT_ERR(rig.ledger->ingest(feltest::make_batch(observations)),
                 ErrorCode::TooManyItems);
}
