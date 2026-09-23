// Fabric Efficiency Ledger - immutable closed periods and correction lineage.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] std::uint64_t revision_total(const PeriodRevision& revision) {
  std::uint64_t total = 0;
  for (const auto& claim : revision.claims) {
    total += claim.cell.known_total;
  }
  return total;
}

}  // namespace

FEL_TEST(integration, closed_revision_stays_readable_after_a_correction) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto original = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                     rig.fixture.counter_incarnation, 1, 100,
                                     Category::UsefulDeliveredWork, rig.fixture.port1);
  original.window_end = rig.fixture.period_start + Duration::from_minutes(5);
  original.observed_at = original.window_end;
  original.received_at = original.window_end;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(original)})));
  auto first = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(first.has_value());
  const std::string original_digest = first.value().content_digest.to_hex();

  // Late evidence in a different reporting window, so the correction adds work.
  auto late = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 2, 250,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  late.window_start = rig.fixture.period_start + Duration::from_minutes(10);
  late.window_end = rig.fixture.period_start + Duration::from_minutes(15);
  late.observed_at = late.window_end;
  late.received_at = late.window_end;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(late)})));
  auto corrected = rig.ledger->correct_period(
      CorrectionRequest{rig.period, rig.fixture.period_end + Duration::from_hours(1), "late",
                        "operator", false});
  FEL_REQUIRE(corrected.has_value());
  FEL_EXPECT_EQ(corrected.value().revision, RevisionOrdinal{2});
  FEL_REQUIRE(corrected.value().parent_digest.has_value());
  FEL_EXPECT_EQ(corrected.value().parent_digest->to_hex(), original_digest);
  FEL_EXPECT_EQ(revision_total(corrected.value()), std::uint64_t{350});

  auto history = rig.ledger->revisions_of(rig.period);
  FEL_REQUIRE(history.size() == 2);
  FEL_EXPECT_EQ(history[0].revision, RevisionOrdinal{1});
  FEL_EXPECT_EQ(history[0].content_digest.to_hex(), original_digest);
  FEL_EXPECT_EQ(revision_total(history[0]), std::uint64_t{100});
  FEL_EXPECT_EQ(history[1].parent_digest->to_hex(), history[0].content_digest.to_hex());

  auto corrections = rig.ledger->corrections_of(rig.period);
  FEL_REQUIRE(corrections.size() == 1);
  FEL_EXPECT_EQ(corrections[0].from_revision, RevisionOrdinal{1});
  FEL_EXPECT_EQ(corrections[0].to_revision, RevisionOrdinal{2});
  FEL_EXPECT_EQ(corrections[0].parent_digest.to_hex(), original_digest);
}

FEL_TEST(integration, revision_digest_detects_tampering) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(
      feltest::make_spec(rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation,
                         1, 100, Category::UsefulDeliveredWork, rig.fixture.port1))})));
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_ASSERT_OK(closed.value().verify_digest());

  PeriodRevision tampered = closed.value();
  FEL_REQUIRE(!tampered.claims.empty());
  tampered.claims.front().cell.known_total += 1;
  FEL_ASSERT_ERR(tampered.verify_digest(), ErrorCode::RecordChecksumMismatch);
}

FEL_TEST(integration, correction_chain_must_be_linear) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  for (int i = 0; i < 3; ++i) {
    FEL_ASSERT_OK(rig.ledger->correct_period(CorrectionRequest{
        rig.period, rig.fixture.period_end + Duration::from_hours(i + 1), "correction", "test",
        false}));
  }
  auto corrections = rig.ledger->corrections_of(rig.period);
  FEL_REQUIRE(corrections.size() == 3);
  for (std::size_t i = 1; i < corrections.size(); ++i) {
    FEL_REQUIRE(corrections[i].previous_correction.has_value());
    FEL_EXPECT_EQ(corrections[i].previous_correction->to_hex(),
                  corrections[i - 1].new_digest.to_hex());
  }
  auto integrity = rig.ledger->verify_integrity();
  FEL_REQUIRE(integrity.has_value());
  FEL_EXPECT_EQ(integrity.value().lineage_breaks, std::uint64_t{0});
  // Three revision links plus two correction-chain links are verified.
  FEL_EXPECT(integrity.value().lineages_verified >= 3);
}

FEL_TEST(integration, correction_budget_is_enforced) {
  auto policy = feltest::test_policy();
  policy.max_corrections_per_period = 2;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  FEL_ASSERT_OK(rig.ledger->correct_period(CorrectionRequest{
      rig.period, rig.fixture.period_end + Duration::from_hours(1), "one", "test", false}));
  FEL_ASSERT_OK(rig.ledger->correct_period(CorrectionRequest{
      rig.period, rig.fixture.period_end + Duration::from_hours(2), "two", "test", false}));
  FEL_ASSERT_ERR(rig.ledger->correct_period(CorrectionRequest{
                     rig.period, rig.fixture.period_end + Duration::from_hours(3), "three", "test",
                     false}),
                 ErrorCode::CorrectionLimitReached);
}

FEL_TEST(integration, open_period_cannot_be_corrected) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_ERR(rig.ledger->correct_period(CorrectionRequest{
                     rig.period, rig.fixture.period_end, "premature", "test", false}),
                 ErrorCode::PeriodNotClosed);
}

FEL_TEST(integration, late_evidence_requires_a_correction_when_policy_says_so) {
  auto policy = feltest::test_policy();
  policy.late_evidence = LateEvidencePolicy::RequiresCorrection;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  auto report = rig.ledger->ingest(feltest::make_batch({feltest::make_observation(
      feltest::make_spec(rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation,
                         9, 500, Category::UsefulDeliveredWork, rig.fixture.port1))}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().late_evidence, std::uint64_t{1});
  FEL_EXPECT_EQ(report.value().accepted, std::uint64_t{1});

  // The closed revision is untouched until a correction is requested.
  auto latest = rig.ledger->latest_revision(rig.period);
  FEL_REQUIRE(latest.has_value());
  FEL_EXPECT_EQ(revision_total(latest.value()), std::uint64_t{0});

  auto corrected = rig.ledger->correct_period(CorrectionRequest{
      rig.period, rig.fixture.period_end + Duration::from_hours(1), "late evidence", "test", false});
  FEL_REQUIRE(corrected.has_value());
  FEL_EXPECT_EQ(revision_total(corrected.value()), std::uint64_t{500});
}
