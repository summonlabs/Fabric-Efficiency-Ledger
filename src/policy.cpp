// Fabric Efficiency Ledger - deterministic accounting policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/policy.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "fel/serialize.hpp"
#include "fel/version.hpp"

namespace fel {
namespace {

constexpr const char* kConflictNames[] = {"record-and-unknown", "strict-fail",
                                          "prefer-higher-authority"};
constexpr const char* kResidualNames[] = {"attribute-to-unknown", "strict-reject"};
constexpr const char* kExcessNames[] = {"record-violation-and-flag", "strict-reject"};
constexpr const char* kStaleNames[] = {"exclude", "include-marked"};
constexpr const char* kLateNames[] = {"reject", "requires-correction"};
constexpr const char* kRecoveryNames[] = {"conservative", "strict"};
constexpr const char* kTieBreakNames[] = {"reject", "lowest-source-id-wins", "highest-source-id-wins"};

[[nodiscard]] Result<void> read_bool(const Json& document, const char* key, bool* out) {
  const Json* value = document.find(key);
  if (value == nullptr) {
    return ok();
  }
  if (!value->is_bool()) {
    return make_error(ErrorCode::SchemaViolation, "policy field must be a boolean", key);
  }
  *out = value->as_bool();
  return ok();
}

[[nodiscard]] Result<void> read_u64(const Json& document, const char* key, std::uint64_t* out) {
  const Json* value = document.find(key);
  if (value == nullptr) {
    return ok();
  }
  auto parsed = value->to_u64();
  if (!parsed.has_value()) {
    return parsed.error();
  }
  *out = parsed.value();
  return ok();
}

[[nodiscard]] Result<void> read_enum(const Json& document, const char* key,
                                     const char* const* names, std::size_t count,
                                     std::size_t* out) {
  const Json* value = document.find(key);
  if (value == nullptr) {
    return ok();
  }
  if (!value->is_string()) {
    return make_error(ErrorCode::SchemaViolation, "policy field must be a string", key);
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (value->as_string() == names[i]) {
      *out = i + 1;
      return ok();
    }
  }
  return make_error(ErrorCode::InvalidArgument, "unknown policy value",
                    std::string(key) + "=" + value->as_string());
}

}  // namespace

std::string_view conflict_policy_name(ConflictPolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 3) return "invalid";
  return kConflictNames[index - 1];
}

std::string_view residual_policy_name(ResidualPolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 2) return "invalid";
  return kResidualNames[index - 1];
}

std::string_view excess_policy_name(ExcessPolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 2) return "invalid";
  return kExcessNames[index - 1];
}

std::string_view stale_policy_name(StaleEvidencePolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 2) return "invalid";
  return kStaleNames[index - 1];
}

std::string_view late_policy_name(LateEvidencePolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 2) return "invalid";
  return kLateNames[index - 1];
}

std::string_view recovery_policy_name(RecoveryPolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 2) return "invalid";
  return kRecoveryNames[index - 1];
}

std::string_view tie_break_policy_name(TieBreakPolicy value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > 3) return "invalid";
  return kTieBreakNames[index - 1];
}

Result<void> LedgerPolicy::validate() const {
  if (format_version != kPolicyFormatVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion, "unsupported policy format version",
                      std::to_string(format_version));
  }
  if (freshness.fresh_ttl.is_negative()) {
    return make_error(ErrorCode::InvalidArgument, "fresh_ttl must not be negative");
  }
  if (freshness.stale_ttl < freshness.fresh_ttl) {
    return make_error(ErrorCode::InvalidArgument,
                      "stale_ttl must be greater than or equal to fresh_ttl");
  }
  if (freshness.clock_skew_allowance.is_negative()) {
    return make_error(ErrorCode::InvalidArgument, "clock_skew_allowance must not be negative");
  }
  if (workers == 0 || workers > limits::kMaxWorkers) {
    return make_error(ErrorCode::InvalidArgument, "workers is outside the permitted range");
  }
  if (queue_depth == 0 || queue_depth > limits::kMaxQueueDepth) {
    return make_error(ErrorCode::InvalidArgument, "queue_depth is outside the permitted range");
  }
  if (max_batch_records == 0 || max_batch_records > limits::kMaxBatchRecords) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_batch_records is outside the permitted range");
  }
  if (max_result_rows == 0 || max_result_rows > limits::kMaxResultRows) {
    return make_error(ErrorCode::InvalidArgument, "max_result_rows is outside the permitted range");
  }
  if (max_explanation_entries == 0 || max_explanation_entries > limits::kMaxExplanationEntries) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_explanation_entries is outside the permitted range");
  }
  if (max_conflict_records == 0 || max_conflict_records > limits::kMaxConflictRecords) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_conflict_records is outside the permitted range");
  }
  if (max_aggregation_buckets == 0 || max_aggregation_buckets > limits::kMaxAggregationBuckets) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_aggregation_buckets is outside the permitted range");
  }
  if (max_corrections_per_period == 0 ||
      max_corrections_per_period > limits::kMaxCorrectionsPerPeriod) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_corrections_per_period is outside the permitted range");
  }
  if (max_period_span.nanos() < limits::kMinWindowNanos ||
      max_period_span.nanos() > limits::kMaxWindowNanos) {
    return make_error(ErrorCode::InvalidArgument, "max_period_span is outside the permitted range");
  }
  if (max_lookback.is_negative() || max_lookback > Duration::from_days(36500)) {
    return make_error(ErrorCode::InvalidArgument, "max_lookback is outside the permitted range");
  }
  if (max_segment_bytes == 0 || max_segment_bytes > limits::kMaxSegmentBytes) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_segment_bytes is outside the permitted range");
  }
  if (max_store_bytes == 0 || max_store_bytes > limits::kMaxStoreBytes) {
    return make_error(ErrorCode::InvalidArgument, "max_store_bytes is outside the permitted range");
  }
  if (max_segments == 0 || max_segments > limits::kMaxSegments) {
    return make_error(ErrorCode::InvalidArgument, "max_segments is outside the permitted range");
  }
  if (max_retained_observations == 0 ||
      max_retained_observations > limits::kMaxRetainedObservations) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_retained_observations is outside the permitted range");
  }
  return ok();
}

std::string LedgerPolicy::canonical_form() const {
  FieldWriter writer;
  writer.field("policy/1");
  writer.field_u64(format_version);
  writer.field_i64(freshness.fresh_ttl.nanos());
  writer.field_i64(freshness.stale_ttl.nanos());
  writer.field_i64(freshness.clock_skew_allowance.nanos());
  writer.field_bool(freshness.require_generation_binding);
  writer.field_bool(freshness.require_incarnation_coverage);
  writer.field_bool(freshness.reject_future_evidence);
  writer.field(conflict_policy_name(conflict));
  writer.field(residual_policy_name(residual));
  writer.field(excess_policy_name(excess));
  writer.field(stale_policy_name(stale));
  writer.field(late_policy_name(late_evidence));
  writer.field(recovery_policy_name(recovery));
  writer.field(tie_break_policy_name(tie_break));
  writer.field_bool(require_total_observations);
  writer.field_u64(max_result_rows);
  writer.field_u64(max_explanation_entries);
  writer.field_u64(max_conflict_records);
  writer.field_u64(max_aggregation_buckets);
  writer.field_i64(max_period_span.nanos());
  writer.field_i64(max_lookback.nanos());
  writer.field_u64(workers);
  writer.field_u64(queue_depth);
  writer.field_u64(max_batch_records);
  writer.field_u64(max_store_bytes);
  writer.field_u64(max_segment_bytes);
  writer.field_u64(max_segments);
  writer.field_u64(max_retained_observations);
  writer.field_u64(max_corrections_per_period);
  return writer.text();
}

Digest256 LedgerPolicy::digest() const { return Sha256::hash(canonical_form()); }

PolicyRevisionId LedgerPolicy::revision_id() const {
  return PolicyRevisionId::derive({"policy", digest().to_hex()});
}

Result<LedgerPolicy> parse_policy(std::string_view json_text) {
  auto parsed_document = Json::parse(json_text);
  if (!parsed_document.has_value()) {
    return parsed_document.error();
  }
  const Json& document = parsed_document.value();
  if (!document.is_object()) {
    return make_error(ErrorCode::SchemaViolation, "policy document must be a JSON object");
  }
  FEL_TRY(document.reject_unknown_keys(
      {"format_version", "freshness", "conflict", "residual", "excess", "stale", "late_evidence",
       "recovery", "tie_break", "require_total_observations", "max_result_rows",
       "max_explanation_entries", "max_conflict_records", "max_aggregation_buckets",
       "max_period_span_ms", "max_lookback_ms", "workers", "queue_depth", "max_batch_records",
       "max_store_bytes", "max_segment_bytes", "max_segments", "max_retained_observations",
       "max_corrections_per_period"}));

  LedgerPolicy policy;
  if (const Json* value = document.find("format_version"); value != nullptr) {
    std::uint64_t parsed = 0;
    FEL_TRY_ASSIGN(parsed, value->to_u64());
    if (parsed > 0xFFFFFFFFULL) {
      return make_error(ErrorCode::SchemaViolation, "format_version is out of range");
    }
    policy.format_version = static_cast<std::uint32_t>(parsed);
  }
  if (const Json* freshness = document.find("freshness"); freshness != nullptr) {
    if (!freshness->is_object()) {
      return make_error(ErrorCode::SchemaViolation, "freshness must be an object");
    }
    FEL_TRY(freshness->reject_unknown_keys({"fresh_ttl_ms", "stale_ttl_ms",
                                            "clock_skew_allowance_ms",
                                            "require_generation_binding",
                                            "require_incarnation_coverage",
                                            "reject_future_evidence"}));
    std::uint64_t millis = 0;
    FEL_TRY(read_u64(*freshness, "fresh_ttl_ms", &millis));
    if (millis != 0) {
      policy.freshness.fresh_ttl = Duration::from_millis(static_cast<std::int64_t>(millis));
    }
    millis = 0;
    FEL_TRY(read_u64(*freshness, "stale_ttl_ms", &millis));
    if (millis != 0) {
      policy.freshness.stale_ttl = Duration::from_millis(static_cast<std::int64_t>(millis));
    }
    millis = 0;
    FEL_TRY(read_u64(*freshness, "clock_skew_allowance_ms", &millis));
    if (millis != 0) {
      policy.freshness.clock_skew_allowance =
          Duration::from_millis(static_cast<std::int64_t>(millis));
    }
    FEL_TRY(read_bool(*freshness, "require_generation_binding",
                      &policy.freshness.require_generation_binding));
    FEL_TRY(read_bool(*freshness, "require_incarnation_coverage",
                      &policy.freshness.require_incarnation_coverage));
    FEL_TRY(read_bool(*freshness, "reject_future_evidence",
                      &policy.freshness.reject_future_evidence));
  }

  std::size_t slot = 0;
  FEL_TRY(read_enum(document, "conflict", kConflictNames, 3, &slot));
  if (slot != 0) policy.conflict = static_cast<ConflictPolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "residual", kResidualNames, 2, &slot));
  if (slot != 0) policy.residual = static_cast<ResidualPolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "excess", kExcessNames, 2, &slot));
  if (slot != 0) policy.excess = static_cast<ExcessPolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "stale", kStaleNames, 2, &slot));
  if (slot != 0) policy.stale = static_cast<StaleEvidencePolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "late_evidence", kLateNames, 2, &slot));
  if (slot != 0) policy.late_evidence = static_cast<LateEvidencePolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "recovery", kRecoveryNames, 2, &slot));
  if (slot != 0) policy.recovery = static_cast<RecoveryPolicy>(slot);
  slot = 0;
  FEL_TRY(read_enum(document, "tie_break", kTieBreakNames, 3, &slot));
  if (slot != 0) policy.tie_break = static_cast<TieBreakPolicy>(slot);

  FEL_TRY(read_bool(document, "require_total_observations", &policy.require_total_observations));

  std::uint64_t value = 0;
  FEL_TRY(read_u64(document, "max_result_rows", &value));
  if (value != 0) policy.max_result_rows = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_explanation_entries", &value));
  if (value != 0) policy.max_explanation_entries = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_conflict_records", &value));
  if (value != 0) policy.max_conflict_records = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_aggregation_buckets", &value));
  if (value != 0) policy.max_aggregation_buckets = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_period_span_ms", &value));
  if (value != 0) policy.max_period_span = Duration::from_millis(static_cast<std::int64_t>(value));
  value = 0;
  FEL_TRY(read_u64(document, "max_lookback_ms", &value));
  if (value != 0) policy.max_lookback = Duration::from_millis(static_cast<std::int64_t>(value));
  value = 0;
  FEL_TRY(read_u64(document, "workers", &value));
  if (value != 0) policy.workers = static_cast<std::uint32_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "queue_depth", &value));
  if (value != 0) policy.queue_depth = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_batch_records", &value));
  if (value != 0) policy.max_batch_records = static_cast<std::size_t>(value);
  value = 0;
  FEL_TRY(read_u64(document, "max_store_bytes", &value));
  if (value != 0) policy.max_store_bytes = value;
  value = 0;
  FEL_TRY(read_u64(document, "max_segment_bytes", &value));
  if (value != 0) policy.max_segment_bytes = value;
  value = 0;
  FEL_TRY(read_u64(document, "max_segments", &value));
  if (value != 0) policy.max_segments = value;
  value = 0;
  FEL_TRY(read_u64(document, "max_retained_observations", &value));
  if (value != 0) policy.max_retained_observations = value;
  value = 0;
  FEL_TRY(read_u64(document, "max_corrections_per_period", &value));
  if (value != 0) policy.max_corrections_per_period = static_cast<std::size_t>(value);

  FEL_TRY(policy.validate());
  return policy;
}

Result<LedgerPolicy> load_policy_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot open policy document", path);
  }
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
    if (text.size() > limits::kMaxRecordBytes) {
      std::fclose(file);
      return make_error(ErrorCode::PayloadTooLarge, "policy document exceeds the payload budget",
                        path);
    }
  }
  std::fclose(file);
  return parse_policy(text);
}

}  // namespace fel
