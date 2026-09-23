// Fabric Efficiency Ledger - adversarial and malformed evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] bool has_unknown(const PeriodRevision& revision, UnknownReason reason) {
  for (const auto& unknown : revision.unknowns) {
    if (unknown.reason == reason) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::uint64_t total_of(const PeriodRevision& revision) {
  std::uint64_t total = 0;
  for (const auto& claim : revision.claims) {
    total += claim.cell.known_total;
  }
  return total;
}

[[nodiscard]] Result<PeriodRevision> close_now(Ledger& ledger, const PeriodId& period,
                                               TimePoint closed_at) {
  return ledger.close_period(CloseRequest{period, closed_at, "close", "test", false});
}

}  // namespace

FEL_TEST(adversarial, sequence_reuse_with_different_content_is_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto first = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                  rig.fixture.counter_incarnation, 5, 100,
                                  Category::UsefulDeliveredWork, rig.fixture.port1);
  auto replay = first;
  replay.amount = 900;  // same sequence, different content
  replay.label = "forged";

  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(
      {feltest::make_observation(first), feltest::make_observation(replay)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  // Neither variant can be trusted, so nothing is counted and the conflict is
  // recorded.
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
  FEL_EXPECT(!closed.value().conflicts.empty());
}

FEL_TEST(adversarial, conflicting_sources_leave_the_cell_unknown) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto claimed_high = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                         rig.fixture.counter_incarnation, 1, 8000,
                                         Category::UsefulDeliveredWork, rig.fixture.port1);
  auto claimed_low = feltest::make_spec(rig.fixture, rig.fixture.probe_source,
                                        rig.fixture.probe_incarnation, 2, 100,
                                        Category::UsefulDeliveredWork, rig.fixture.port1);
  claimed_low.window_start = claimed_high.window_start;
  claimed_low.window_end = claimed_high.window_end;

  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(claimed_high),
                                                        feltest::make_observation(claimed_low)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::ConflictingEvidence));
}

FEL_TEST(adversarial, higher_authority_resolves_a_conflict_deterministically) {
  auto policy = feltest::test_policy();
  policy.conflict = ConflictPolicy::PreferHigherAuthority;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto high = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 8000,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  auto low = feltest::make_spec(rig.fixture, rig.fixture.probe_source,
                                rig.fixture.probe_incarnation, 2, 100,
                                Category::UsefulDeliveredWork, rig.fixture.port1);
  low.window_start = high.window_start;
  low.window_end = high.window_end;

  FEL_ASSERT_OK(rig.ledger->ingest(
      feltest::make_batch({feltest::make_observation(high), feltest::make_observation(low)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{8000});
  FEL_REQUIRE(!closed.value().conflicts.empty());
  FEL_EXPECT(closed.value().conflicts.front().resolved);
}

FEL_TEST(adversarial, strict_conflict_policy_refuses_to_close) {
  auto policy = feltest::test_policy();
  policy.conflict = ConflictPolicy::StrictFail;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto high = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 8000,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  auto low = feltest::make_spec(rig.fixture, rig.fixture.probe_source,
                                rig.fixture.probe_incarnation, 2, 100,
                                Category::UsefulDeliveredWork, rig.fixture.port1);
  low.window_start = high.window_start;
  low.window_end = high.window_end;
  FEL_ASSERT_OK(rig.ledger->ingest(
      feltest::make_batch({feltest::make_observation(high), feltest::make_observation(low)})));
  FEL_ASSERT_ERR(close_now(*rig.ledger, rig.period, rig.fixture.period_end),
                 ErrorCode::ConflictingEvidence);
}

FEL_TEST(adversarial, epoch_regression_inside_one_incarnation_is_fenced) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto old_epoch = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                      rig.fixture.counter_incarnation, 1, 100,
                                      Category::UsefulDeliveredWork, rig.fixture.port1);
  old_epoch.epoch = 1;
  auto new_epoch = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                      rig.fixture.counter_incarnation, 2, 400,
                                      Category::UsefulDeliveredWork, rig.fixture.port1);
  new_epoch.epoch = 7;
  new_epoch.window_start = old_epoch.window_start + Duration::from_minutes(2);
  new_epoch.window_end = new_epoch.window_start + Duration::from_minutes(1);
  new_epoch.observed_at = new_epoch.window_end;
  new_epoch.received_at = new_epoch.window_end;

  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(
      {feltest::make_observation(old_epoch), feltest::make_observation(new_epoch)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{400});
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::FencedEpoch));
}

FEL_TEST(adversarial, epoch_below_the_incarnation_boot_epoch_is_rejected_at_ingest) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  spec.epoch = 0;
  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_epoch_fenced, std::uint64_t{1});
}

FEL_TEST(adversarial, retired_incarnation_is_refused) {
  auto fixture = feltest::make_fixture();
  const SourceId retired_source = SourceId::derive({"source", "retired-agent"});
  const IncarnationId retired_incarnation = IncarnationId::derive({"incarnation", "retired"});

  Source source;
  source.id = retired_source;
  source.label = "retired agent";
  source.kind = SourceKind::SwitchCounter;
  source.authority = AuthorityRank{9};
  source.provenance = ProvenanceClass::Real;
  FEL_REQUIRE(fixture.topology.add_source(source).has_value());

  SourceIncarnation incarnation;
  incarnation.id = retired_incarnation;
  incarnation.source = retired_source;
  incarnation.boot_epoch = EpochId{1};
  incarnation.started_at = fixture.period_start - Duration::from_hours(1);
  incarnation.retired_at = fixture.period_start + Duration::from_minutes(1);
  FEL_REQUIRE(fixture.topology.add_incarnation(incarnation).has_value());
  FEL_REQUIRE(fixture.topology.validate().has_value());

  auto ledger = Ledger::in_memory(feltest::test_policy(), fixture.topology, fixture.period_end);
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(fixture.period_start, fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());

  auto spec = feltest::make_spec(fixture, retired_source, retired_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, fixture.port1);
  // Arrives well after the agent was retired.
  spec.received_at = fixture.period_start + Duration::from_minutes(30);
  spec.observed_at = fixture.period_start + Duration::from_minutes(29);
  auto report = ledger.value()->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_retired_incarnation, std::uint64_t{1});
}

FEL_TEST(adversarial, future_dated_evidence_is_a_clock_anomaly) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  // Observation time after the receive time is impossible.
  spec.observed_at = rig.fixture.period_end + Duration::from_hours(1);
  spec.received_at = rig.fixture.period_start;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::ClockAnomaly));
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
}

FEL_TEST(adversarial, evidence_for_an_undefined_resource_is_refused) {
  // With the default policy the missing generation binding is the first fence.
  {
    auto rig_result = feltest::make_memory_rig();
    FEL_REQUIRE(rig_result.has_value());
    auto rig = std::move(rig_result.value());
    const ResourceId ghost = ResourceId::derive({"resource", "ghost"});
    auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                   rig.fixture.counter_incarnation, 1, 100,
                                   Category::UsefulDeliveredWork, ghost);
    FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));
    auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
    FEL_REQUIRE(closed.has_value());
    FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
    FEL_EXPECT(has_unknown(closed.value(), UnknownReason::StaleGeneration));
  }
  // Without the binding requirement the evidence reaches attribution and is
  // refused there instead. Both paths refuse to count it.
  {
    auto policy = feltest::test_policy();
    policy.freshness.require_generation_binding = false;
    auto rig_result = feltest::make_memory_rig_with(policy);
    FEL_REQUIRE(rig_result.has_value());
    auto rig = std::move(rig_result.value());
    const ResourceId ghost = ResourceId::derive({"resource", "ghost"});
    auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                   rig.fixture.counter_incarnation, 1, 100,
                                   Category::UsefulDeliveredWork, ghost);
    FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));
    auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
    FEL_REQUIRE(closed.has_value());
    FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
    FEL_EXPECT(has_unknown(closed.value(), UnknownReason::AttributionFailure));
  }
}

FEL_TEST(adversarial, a_total_that_declares_a_category_is_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::Retransmission, rig.fixture.port1);
  spec.role = ObservationRole::Total;
  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_shape, std::uint64_t{1});
}

FEL_TEST(adversarial, zero_width_window_and_missing_times_are_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  spec.window_end = spec.window_start;
  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_shape, std::uint64_t{1});
}

FEL_TEST(adversarial, oversized_metadata_is_refused) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto observation = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 100,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  for (std::size_t i = 0; i < limits::kMaxMetadataEntries + 1; ++i) {
    observation.metadata.push_back(
        MetadataEntry{"key-" + std::to_string(i), "value"});
  }
  auto report = rig.ledger->ingest(feltest::make_batch({observation}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_shape, std::uint64_t{1});
}

FEL_TEST(adversarial, two_records_with_the_same_evidence_identity_are_both_dropped) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto first = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 100,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  auto clone = first;
  clone.amount = 5000;  // same identity, different content
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({first, clone})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
}

FEL_TEST(adversarial, a_self_contradicting_source_never_produces_a_partial_total) {
  // One cell and one reporting window, many records from one source with
  // different amounts. The runtime must not pick a winner: the whole cell
  // becomes unknown and every participant is explained.
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  std::vector<Observation> observations;
  for (std::uint64_t i = 0; i < 24; ++i) {
    auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                   rig.fixture.counter_incarnation, i + 1, 1000 + i * 7,
                                   Category::UsefulDeliveredWork, rig.fixture.port1);
    // Identical reporting window for every record.
    spec.window_start = rig.fixture.period_start;
    spec.window_end = rig.fixture.period_start + Duration::from_minutes(10);
    spec.observed_at = spec.window_end;
    spec.received_at = spec.window_end;
    observations.push_back(feltest::make_observation(spec));
  }
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(observations)));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
  FEL_EXPECT(!closed.value().conflicts.empty());
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::ConflictingEvidence));
  // Nothing is invented: the efficiency summary reports no known work.
  for (const auto& summary : closed.value().efficiency) {
    FEL_EXPECT_EQ(summary.useful, std::uint64_t{0});
    FEL_EXPECT_EQ(summary.avoidable, std::uint64_t{0});
  }
}

FEL_TEST(adversarial, one_unmeasured_contribution_makes_the_ratio_indeterminate) {
  // A measured contribution plus a contribution that cannot be used. The
  // measured part is reported, the ratio is not: efficiency is never derived
  // from partial telemetry.
  auto policy = feltest::test_policy();
  policy.freshness.fresh_ttl = Duration::from_minutes(15);
  policy.freshness.stale_ttl = Duration::from_minutes(60);
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto measured = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                     rig.fixture.counter_incarnation, 1, 1000,
                                     Category::UsefulDeliveredWork, rig.fixture.port1);
  measured.observed_at = rig.fixture.period_start + Duration::from_minutes(30);
  measured.received_at = measured.observed_at;
  measured.window_start = rig.fixture.period_start + Duration::from_minutes(25);
  measured.window_end = rig.fixture.period_start + Duration::from_minutes(30);

  auto unusable = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                     rig.fixture.counter_incarnation, 2, 5000,
                                     Category::UsefulDeliveredWork, rig.fixture.port1);
  unusable.observed_at = rig.fixture.period_start + Duration::from_minutes(1);
  unusable.received_at = unusable.observed_at;
  unusable.window_start = rig.fixture.period_start;
  unusable.window_end = rig.fixture.period_start + Duration::from_minutes(5);

  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(
      {feltest::make_observation(measured), feltest::make_observation(unusable)})));

  // Evaluated 40 minutes into the period: the first record is stale and the
  // second is still fresh.
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_start + Duration::from_minutes(40), "close",
                   "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{1000});
  bool saw_indeterminate = false;
  for (const auto& summary : closed.value().efficiency) {
    if (summary.kind != MeasureKind::WireBytes) {
      continue;
    }
    FEL_EXPECT(!summary.determinate);
    FEL_EXPECT(summary.unknown_contributions > 0);
    FEL_EXPECT(summary.coverage != Coverage::Known);
    FEL_EXPECT(summary.ratio_text().find("indeterminate") == 0);
    saw_indeterminate = true;
  }
  FEL_EXPECT(saw_indeterminate);
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::StaleEvidence));
}

FEL_TEST(adversarial, unsupported_source_never_contributes_even_with_top_authority) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.unsupported_source,
                                 rig.fixture.counter_incarnation, 1, 5000,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
  FEL_EXPECT(closed.value().proof_surfaces.empty());
}

FEL_TEST(adversarial, a_huge_amount_is_reported_rather_than_wrapped) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  std::vector<Observation> observations;
  for (std::uint64_t i = 0; i < 4; ++i) {
    auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                   rig.fixture.counter_incarnation, i + 1,
                                   std::numeric_limits<std::uint64_t>::max(),
                                   Category::UsefulDeliveredWork, rig.fixture.port1);
    spec.window_start = rig.fixture.period_start + Duration::from_minutes(
                                                      static_cast<std::int64_t>(i) * 2);
    spec.window_end = spec.window_start + Duration::from_minutes(1);
    spec.observed_at = spec.window_end;
    spec.received_at = spec.window_end;
    observations.push_back(feltest::make_observation(spec));
  }
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(observations)));
  auto closed = close_now(*rig.ledger, rig.period, rig.fixture.period_end);
  // Four maximal amounts cannot be represented: the runtime reports the
  // overflow instead of wrapping around.
  FEL_EXPECT(!closed.has_value());
  if (!closed.has_value()) {
    FEL_EXPECT_EQ(closed.error().code, ErrorCode::ArithmeticOverflow);
  }
}

FEL_TEST(adversarial, stale_evidence_is_excluded_and_expired_evidence_is_never_counted) {
  auto policy = feltest::test_policy();
  policy.freshness.fresh_ttl = Duration::from_minutes(1);
  policy.freshness.stale_ttl = Duration::from_minutes(5);
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto spec = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(spec)})));

  // Evaluated an hour after the observation, the evidence is expired.
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_start + Duration::from_hours(1), "close", "test",
                   false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(total_of(closed.value()), std::uint64_t{0});
  FEL_EXPECT(has_unknown(closed.value(), UnknownReason::ExpiredEvidence));

  // Evaluated two minutes after the observation, the evidence is stale, and the
  // default policy excludes stale evidence from totals as well.
  auto fresh_ledger = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(fresh_ledger.has_value());
  auto fresh_rig = std::move(fresh_ledger.value());
  auto late = feltest::make_spec(fresh_rig.fixture, fresh_rig.fixture.counter_source,
                                 fresh_rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, fresh_rig.fixture.port1);
  // Observed two minutes into the period, evaluated five minutes in: older than
  // the one minute freshness budget but inside the five minute retention one.
  late.observed_at = fresh_rig.fixture.period_start + Duration::from_minutes(2);
  late.received_at = late.observed_at;
  late.window_start = fresh_rig.fixture.period_start;
  late.window_end = fresh_rig.fixture.period_start + Duration::from_minutes(5);
  FEL_ASSERT_OK(fresh_rig.ledger->ingest(feltest::make_batch({feltest::make_observation(late)})));
  auto stale_close = fresh_rig.ledger->close_period(
      CloseRequest{fresh_rig.period, fresh_rig.fixture.period_start + Duration::from_minutes(5),
                   "close", "test", false});
  FEL_REQUIRE(stale_close.has_value());
  FEL_EXPECT_EQ(total_of(stale_close.value()), std::uint64_t{0});
  FEL_EXPECT(has_unknown(stale_close.value(), UnknownReason::StaleEvidence));
}
