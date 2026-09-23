// Fabric Efficiency Ledger - explanations, exports and deterministic ordering.
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

[[nodiscard]] std::vector<Observation> sample_evidence(const feltest::Fixture& fixture) {
  std::vector<Observation> observations;
  observations.push_back(feltest::make_observation(feltest::make_spec(
      fixture, fixture.counter_source, fixture.counter_incarnation, 1, 900,
      Category::UsefulDeliveredWork, fixture.port1)));
  observations.push_back(feltest::make_observation(feltest::make_spec(
      fixture, fixture.counter_source, fixture.counter_incarnation, 2, 100,
      Category::Retransmission, fixture.port1)));
  observations.push_back(feltest::make_observation(feltest::make_spec(
      fixture, fixture.probe_source, fixture.probe_incarnation, 3, 250,
      Category::UsefulDeliveredWork, fixture.port2)));
  return observations;
}

}  // namespace

FEL_TEST(integration, explanation_traces_a_cell_to_policy_category_and_conservation) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(sample_evidence(rig.fixture))));
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_REQUIRE(!closed.value().claims.empty());

  const auto& claim = closed.value().claims.front();
  ExplainRequest request;
  request.period = rig.period;
  request.identity = claim.identity;
  auto explanation = rig.ledger->explain(request);
  FEL_REQUIRE(explanation.has_value());
  FEL_EXPECT(explanation.value().found);
  FEL_EXPECT_EQ(explanation.value().cell.cell.known_total, claim.cell.known_total);
  FEL_EXPECT(!explanation.value().derivation.empty());

  bool saw_category = false;
  bool saw_conservation = false;
  bool saw_double_count = false;
  for (const auto& step : explanation.value().derivation) {
    if (step.rule == "category") saw_category = true;
    if (step.rule == "conservation") saw_conservation = true;
    if (step.rule == "double-count") saw_double_count = true;
  }
  FEL_EXPECT(saw_category);
  FEL_EXPECT(saw_conservation);
  FEL_EXPECT(saw_double_count);
}

FEL_TEST(integration, explanation_is_deterministic_and_sorted) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(sample_evidence(rig.fixture))));
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());

  ExplainRequest request;
  request.period = rig.period;
  request.identity = closed.value().claims.front().identity;
  auto first = rig.ledger->explain(request);
  auto second = rig.ledger->explain(request);
  FEL_REQUIRE(first.has_value());
  FEL_REQUIRE(second.has_value());
  FEL_EXPECT_EQ(first.value().canonical_form(), second.value().canonical_form());
  FEL_EXPECT_EQ(first.value().digest.to_hex(), second.value().digest.to_hex());
  FEL_EXPECT(std::is_sorted(first.value().derivation.begin(), first.value().derivation.end()));
}

FEL_TEST(integration, explanation_of_an_unknown_identity_is_explicit) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));
  ExplainRequest request;
  request.period = rig.period;
  request.identity = AccountingIdentity::from_digest(Sha256::hash("no such cell"));
  auto explanation = rig.ledger->explain(request);
  FEL_REQUIRE(explanation.has_value());
  FEL_EXPECT(!explanation.value().found);
  FEL_EXPECT(!explanation.value().reason.empty());
}

FEL_TEST(integration, export_formats_agree_on_the_underlying_rows) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(sample_evidence(rig.fixture))));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  ExportRequest request;
  request.period = rig.period;
  request.format = ExportFormat::Csv;
  auto csv = rig.ledger->export_period(request);
  FEL_REQUIRE(csv.has_value());
  FEL_EXPECT_EQ(csv.value().rows, std::uint64_t{3});
  FEL_EXPECT(csv.value().text.find("useful-delivered-work") != std::string::npos);

  request.format = ExportFormat::JsonLines;
  auto jsonl = rig.ledger->export_period(request);
  FEL_REQUIRE(jsonl.has_value());
  FEL_EXPECT_EQ(jsonl.value().rows, std::uint64_t{3});

  request.format = ExportFormat::CanonicalJson;
  auto canonical = rig.ledger->export_period(request);
  FEL_REQUIRE(canonical.has_value());
  FEL_EXPECT_EQ(canonical.value().rows, std::uint64_t{3});

  // The payload digests differ because the encodings differ, but repeating the
  // same export must reproduce the same digest exactly.
  auto repeated = rig.ledger->export_period(request);
  FEL_REQUIRE(repeated.has_value());
  FEL_EXPECT_EQ(canonical.value().payload_digest.to_hex(), repeated.value().payload_digest.to_hex());
}

FEL_TEST(integration, export_marks_provenance_and_truncation) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto evidence = sample_evidence(rig.fixture);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(evidence)));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  ExportRequest request;
  request.period = rig.period;
  request.format = ExportFormat::JsonLines;
  request.max_rows = 1;
  auto result = rig.ledger->export_period(request);
  FEL_REQUIRE(result.has_value());
  FEL_EXPECT_EQ(result.value().rows, std::uint64_t{1});
  FEL_EXPECT(result.value().truncation.truncated);
  FEL_EXPECT_EQ(result.value().stamp.proof_surface_label(), std::string("real+synthetic"));
  FEL_EXPECT(!result.value().stamp.purely_real());
}

FEL_TEST(integration, query_respects_the_row_budget_and_reports_it) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(sample_evidence(rig.fixture))));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  QueryRequest request;
  request.period = rig.period;
  request.max_rows = 2;
  auto summary = rig.ledger->query(request);
  FEL_REQUIRE(summary.has_value());
  FEL_EXPECT_EQ(summary.value().rows.size(), std::size_t{2});
  FEL_EXPECT(summary.value().truncation.truncated);
  FEL_EXPECT_EQ(summary.value().truncation.total_available, std::uint64_t{3});
}

FEL_TEST(integration, query_can_select_a_specific_historical_revision) {
  auto policy = feltest::test_policy();
  policy.late_evidence = LateEvidencePolicy::RequiresCorrection;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  auto first = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                 rig.fixture.counter_incarnation, 1, 100,
                                 Category::UsefulDeliveredWork, rig.fixture.port1);
  first.window_end = rig.fixture.period_start + Duration::from_minutes(5);
  first.observed_at = first.window_end;
  first.received_at = first.window_end;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(first)})));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  // A second, independent reporting window so that the correction adds work
  // rather than creating a conflict in the first window.
  auto second = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                   rig.fixture.counter_incarnation, 2, 900,
                                   Category::UsefulDeliveredWork, rig.fixture.port1);
  second.window_start = rig.fixture.period_start + Duration::from_minutes(10);
  second.window_end = rig.fixture.period_start + Duration::from_minutes(15);
  second.observed_at = second.window_end;
  second.received_at = second.window_end;
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({feltest::make_observation(second)})));
  FEL_ASSERT_OK(rig.ledger->correct_period(
      CorrectionRequest{rig.period, rig.fixture.period_end + Duration::from_hours(1), "late",
                        "test", false}));

  QueryRequest request;
  request.period = rig.period;
  request.revision = RevisionOrdinal{1};
  auto original = rig.ledger->query(request);
  FEL_REQUIRE(original.has_value());
  std::uint64_t original_total = 0;
  for (const auto& row : original.value().rows) {
    original_total += row.cell.known_total;
  }
  FEL_EXPECT_EQ(original_total, std::uint64_t{100});

  request.revision.reset();
  auto latest = rig.ledger->query(request);
  FEL_REQUIRE(latest.has_value());
  std::uint64_t latest_total = 0;
  for (const auto& row : latest.value().rows) {
    latest_total += row.cell.known_total;
  }
  FEL_EXPECT_EQ(latest_total, std::uint64_t{1000});
  FEL_EXPECT_EQ(original.value().content_digest.to_hex().empty(), false);
}
