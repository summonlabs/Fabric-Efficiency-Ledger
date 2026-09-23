// Fabric Efficiency Ledger - deterministic accounting policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fel/id.hpp"
#include "fel/limits.hpp"
#include "fel/measure.hpp"
#include "fel/time.hpp"
#include "fel/version.hpp"

namespace fel {

// What to do when two sources of equal standing disagree about the same
// accounting cell. The default is to refuse to guess: the cell becomes unknown
// and the conflict is recorded.
enum class ConflictPolicy : std::uint8_t {
  RecordAndUnknown = 1,
  StrictFail = 2,
  PreferHigherAuthority = 3,
};

// What to do with the difference between the observed total and the sum of
// attributed categories.
enum class ResidualPolicy : std::uint8_t {
  AttributeToUnknown = 1,  // residual lands in the explicit unknown bucket
  StrictReject = 2,        // closing fails while a residual exists
};

// What to do when attributed categories exceed the observed total.
enum class ExcessPolicy : std::uint8_t {
  RecordViolationAndFlagIndeterminate = 1,
  StrictReject = 2,
};

enum class StaleEvidencePolicy : std::uint8_t {
  Exclude = 1,        // stale evidence is excluded and recorded as unknown
  IncludeMarked = 2,  // stale evidence counts but every result is flagged
};

enum class LateEvidencePolicy : std::uint8_t {
  Reject = 1,               // evidence for a closed period is refused
  RequiresCorrection = 2,   // evidence is stored and a correction is required
};

enum class RecoveryPolicy : std::uint8_t {
  Conservative = 1,  // recover the longest valid prefix, report all damage
  Strict = 2,        // any damage fails the open
};

enum class TieBreakPolicy : std::uint8_t {
  Reject = 1,                     // equal authority disagreement is unresolved
  LowestSourceIdWins = 2,         // deterministic, but still recorded as a conflict
  HighestSourceIdWins = 3,
};

[[nodiscard]] std::string_view conflict_policy_name(ConflictPolicy value) noexcept;
[[nodiscard]] std::string_view residual_policy_name(ResidualPolicy value) noexcept;
[[nodiscard]] std::string_view excess_policy_name(ExcessPolicy value) noexcept;
[[nodiscard]] std::string_view stale_policy_name(StaleEvidencePolicy value) noexcept;
[[nodiscard]] std::string_view late_policy_name(LateEvidencePolicy value) noexcept;
[[nodiscard]] std::string_view recovery_policy_name(RecoveryPolicy value) noexcept;
[[nodiscard]] std::string_view tie_break_policy_name(TieBreakPolicy value) noexcept;

struct FreshnessPolicy {
  // Evidence is fresh while (as_of - observed_at) <= fresh_ttl.
  Duration fresh_ttl = Duration::from_minutes(5);
  // Beyond stale_ttl the evidence is expired and never usable.
  Duration stale_ttl = Duration::from_hours(1);
  // Tolerance applied before an observation is called "from the future".
  Duration clock_skew_allowance = Duration::from_seconds(30);
  bool require_generation_binding = true;
  bool require_incarnation_coverage = true;
  bool reject_future_evidence = true;
};

struct LedgerPolicy {
  std::uint32_t format_version = kPolicyFormatVersion;

  FreshnessPolicy freshness{};
  ConflictPolicy conflict = ConflictPolicy::RecordAndUnknown;
  ResidualPolicy residual = ResidualPolicy::AttributeToUnknown;
  ExcessPolicy excess = ExcessPolicy::RecordViolationAndFlagIndeterminate;
  StaleEvidencePolicy stale = StaleEvidencePolicy::Exclude;
  LateEvidencePolicy late_evidence = LateEvidencePolicy::RequiresCorrection;
  RecoveryPolicy recovery = RecoveryPolicy::Conservative;
  TieBreakPolicy tie_break = TieBreakPolicy::Reject;

  // When true a period cannot be closed without an independent total for every
  // domain that has contributions. Conservation is then a proof obligation.
  bool require_total_observations = false;

  // Aggregation and reporting bounds.
  std::size_t max_result_rows = limits::kMaxResultRows;
  std::size_t max_explanation_entries = limits::kMaxExplanationEntries;
  std::size_t max_conflict_records = limits::kMaxConflictRecords;
  std::size_t max_aggregation_buckets = limits::kMaxAggregationBuckets;

  // Period geometry bounds.
  Duration max_period_span = Duration::from_days(366);
  Duration max_lookback = Duration::from_days(3650);

  // Runtime bounds.
  std::uint32_t workers = 4;
  std::size_t queue_depth = 4096;
  std::size_t max_batch_records = limits::kMaxBatchRecords;

  // Persistence bounds.
  std::uint64_t max_store_bytes = limits::kMaxStoreBytes;
  std::uint64_t max_segment_bytes = limits::kMaxSegmentBytes;
  std::uint64_t max_segments = limits::kMaxSegments;
  std::uint64_t max_retained_observations = limits::kMaxRetainedObservations;
  std::size_t max_corrections_per_period = limits::kMaxCorrectionsPerPeriod;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] Digest256 digest() const;
  [[nodiscard]] PolicyRevisionId revision_id() const;
};

// Strict parser for the versioned policy document. Unknown fields are refused so
// that a typo can never silently change accounting behaviour.
[[nodiscard]] Result<LedgerPolicy> parse_policy(std::string_view json_text);
[[nodiscard]] Result<LedgerPolicy> load_policy_file(const std::string& path);

}  // namespace fel
