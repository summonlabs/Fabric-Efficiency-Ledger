// Fabric Efficiency Ledger - policy validation, digest and strict parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fel/policy.hpp"
#include "fel/serialize.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, default_policy_validates_and_is_content_addressed) {
  const LedgerPolicy policy;
  FEL_ASSERT_OK(policy.validate());
  const LedgerPolicy other;
  FEL_EXPECT_EQ(policy.digest().to_hex(), other.digest().to_hex());
  FEL_EXPECT_EQ(policy.revision_id(), other.revision_id());
}

FEL_TEST(unit, one_field_change_changes_the_digest) {
  LedgerPolicy base;
  LedgerPolicy changed = base;
  changed.conflict = ConflictPolicy::StrictFail;
  FEL_EXPECT(base.digest() != changed.digest());
  FEL_EXPECT(base.revision_id() != changed.revision_id());

  LedgerPolicy timing = base;
  timing.freshness.fresh_ttl = Duration::from_seconds(17);
  FEL_EXPECT(base.digest() != timing.digest());

  LedgerPolicy budget = base;
  budget.max_result_rows = base.max_result_rows - 1;
  FEL_EXPECT(base.digest() != budget.digest());
}

FEL_TEST(unit, policy_validation_rejects_out_of_range_fields) {
  LedgerPolicy policy;
  policy.workers = 0;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.workers = limits::kMaxWorkers + 1;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.queue_depth = 0;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.max_result_rows = 0;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.max_segment_bytes = limits::kMaxSegmentBytes + 1;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.freshness.fresh_ttl = Duration::from_hours(2);
  policy.freshness.stale_ttl = Duration::from_hours(1);
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::InvalidArgument);

  policy = LedgerPolicy{};
  policy.format_version = 99;
  FEL_ASSERT_ERR(policy.validate(), ErrorCode::UnsupportedFormatVersion);
}

FEL_TEST(unit, policy_parsing_applies_every_field) {
  const std::string text = R"({
    "format_version": 1,
    "freshness": {"fresh_ttl_ms": 1000, "stale_ttl_ms": 4000, "clock_skew_allowance_ms": 5,
                  "require_generation_binding": false,
                  "require_incarnation_coverage": false,
                  "reject_future_evidence": false},
    "conflict": "strict-fail",
    "residual": "strict-reject",
    "excess": "strict-reject",
    "stale": "include-marked",
    "late_evidence": "reject",
    "recovery": "strict",
    "tie_break": "lowest-source-id-wins",
    "require_total_observations": true,
    "max_result_rows": 11,
    "max_explanation_entries": 12,
    "max_conflict_records": 13,
    "max_aggregation_buckets": 14,
    "max_period_span_ms": 1000000,
    "max_lookback_ms": 2000000,
    "workers": 2,
    "queue_depth": 8,
    "max_batch_records": 16,
    "max_store_bytes": 1048576,
    "max_segment_bytes": 4096,
    "max_segments": 4,
    "max_retained_observations": 32,
    "max_corrections_per_period": 3
  })";
  auto policy = parse_policy(text);
  FEL_REQUIRE(policy.has_value());
  FEL_EXPECT_EQ(policy.value().freshness.fresh_ttl, Duration::from_millis(1000));
  FEL_EXPECT_EQ(policy.value().freshness.stale_ttl, Duration::from_millis(4000));
  FEL_EXPECT_EQ(policy.value().freshness.clock_skew_allowance, Duration::from_millis(5));
  FEL_EXPECT(!policy.value().freshness.require_generation_binding);
  FEL_EXPECT(!policy.value().freshness.require_incarnation_coverage);
  FEL_EXPECT(!policy.value().freshness.reject_future_evidence);
  FEL_EXPECT_EQ(policy.value().conflict, ConflictPolicy::StrictFail);
  FEL_EXPECT_EQ(policy.value().residual, ResidualPolicy::StrictReject);
  FEL_EXPECT_EQ(policy.value().excess, ExcessPolicy::StrictReject);
  FEL_EXPECT_EQ(policy.value().stale, StaleEvidencePolicy::IncludeMarked);
  FEL_EXPECT_EQ(policy.value().late_evidence, LateEvidencePolicy::Reject);
  FEL_EXPECT_EQ(policy.value().recovery, RecoveryPolicy::Strict);
  FEL_EXPECT_EQ(policy.value().tie_break, TieBreakPolicy::LowestSourceIdWins);
  FEL_EXPECT(policy.value().require_total_observations);
  FEL_EXPECT_EQ(policy.value().max_result_rows, std::size_t{11});
  FEL_EXPECT_EQ(policy.value().max_aggregation_buckets, std::size_t{14});
  FEL_EXPECT_EQ(policy.value().workers, std::uint32_t{2});
  FEL_EXPECT_EQ(policy.value().queue_depth, std::size_t{8});
  FEL_EXPECT_EQ(policy.value().max_segments, std::uint64_t{4});
  FEL_EXPECT_EQ(policy.value().max_corrections_per_period, std::size_t{3});
}

FEL_TEST(unit, policy_parsing_is_strict) {
  FEL_ASSERT_ERR(parse_policy("{\"unknown_field\": 1}"), ErrorCode::UnknownField);
  FEL_ASSERT_ERR(parse_policy("{\"conflict\": \"nonsense\"}"), ErrorCode::InvalidArgument);
  FEL_ASSERT_ERR(parse_policy("{\"conflict\": 5}"), ErrorCode::SchemaViolation);
  FEL_ASSERT_ERR(parse_policy("{\"freshness\": {\"nope\": 1}}"), ErrorCode::UnknownField);
  FEL_ASSERT_ERR(parse_policy("not json"), ErrorCode::MalformedJson);
  FEL_ASSERT_ERR(parse_policy("[]"), ErrorCode::SchemaViolation);
  FEL_ASSERT_ERR(parse_policy("{\"workers\": 1000}"), ErrorCode::InvalidArgument);
}

FEL_TEST(unit, parsed_policy_matches_the_default_digest) {
  const LedgerPolicy defaults;
  // The defaults expressed as a document must reconstruct the exact defaults.
  const std::string text = R"({
    "freshness": {"fresh_ttl_ms": 300000, "stale_ttl_ms": 3600000,
                  "clock_skew_allowance_ms": 30000},
    "workers": 4,
    "queue_depth": 4096
  })";
  auto policy = parse_policy(text);
  FEL_REQUIRE(policy.has_value());
  FEL_EXPECT_EQ(policy.value().freshness.fresh_ttl, defaults.freshness.fresh_ttl);
  FEL_EXPECT_EQ(policy.value().freshness.stale_ttl, defaults.freshness.stale_ttl);
  FEL_EXPECT_EQ(policy.value().workers, defaults.workers);
  FEL_EXPECT_EQ(policy.value().queue_depth, defaults.queue_depth);
  FEL_EXPECT_EQ(policy.value().digest().to_hex(), defaults.digest().to_hex());
}

FEL_TEST(unit, policy_enum_names_round_trip) {
  FEL_EXPECT_EQ(std::string(conflict_policy_name(ConflictPolicy::RecordAndUnknown)),
                std::string("record-and-unknown"));
  FEL_EXPECT_EQ(std::string(residual_policy_name(ResidualPolicy::AttributeToUnknown)),
                std::string("attribute-to-unknown"));
  FEL_EXPECT_EQ(std::string(excess_policy_name(ExcessPolicy::RecordViolationAndFlagIndeterminate)),
                std::string("record-violation-and-flag"));
  FEL_EXPECT_EQ(std::string(stale_policy_name(StaleEvidencePolicy::Exclude)),
                std::string("exclude"));
  FEL_EXPECT_EQ(std::string(late_policy_name(LateEvidencePolicy::RequiresCorrection)),
                std::string("requires-correction"));
  FEL_EXPECT_EQ(std::string(recovery_policy_name(RecoveryPolicy::Conservative)),
                std::string("conservative"));
  FEL_EXPECT_EQ(std::string(tie_break_policy_name(TieBreakPolicy::Reject)), std::string("reject"));
}
