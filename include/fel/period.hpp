// Fabric Efficiency Ledger - accounting periods, revisions, corrections.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/claim.hpp"
#include "fel/id.hpp"
#include "fel/measure.hpp"
#include "fel/time.hpp"
#include "fel/topology.hpp"

namespace fel {

enum class PeriodState : std::uint8_t {
  Open = 1,
  Closed = 2,
  Superseded = 3,  // a later revision exists; this one stays readable forever
};

[[nodiscard]] std::string_view period_state_name(PeriodState state) noexcept;

// A half open accounting interval [start, end). Bounded by policy.
struct AccountingPeriod {
  PeriodId id;
  Ordinal ordinal;
  std::string label;
  TimePoint start;
  TimePoint end;
  PeriodState state = PeriodState::Open;
  RevisionOrdinal current_revision;
  std::uint64_t observations_ingested = 0;

  [[nodiscard]] bool contains(TimePoint when) const noexcept {
    return !(when < start) && when < end;
  }
  [[nodiscard]] Duration span() const noexcept { return end - start; }
  [[nodiscard]] std::string canonical_form() const;
};

// The outcome of one conservation equation. Conservation is only ever evaluated
// inside a single (scope, generation, measure kind) domain. Mixing domains is an
// error, not a rounding problem.
enum class ConservationStatus : std::uint8_t {
  Consistent = 1,    // attributed == observed
  Residual = 2,      // attributed < observed; the difference is unknown work
  Excess = 3,        // attributed > observed; the evidence contradicts itself
  Unverifiable = 4,  // no independent total was reported for this domain
};

inline constexpr std::size_t kConservationStatusCount = 4;

[[nodiscard]] std::string_view conservation_status_name(ConservationStatus status) noexcept;

struct DomainConservation {
  ScopeId scope;
  GenerationId generation;
  MeasureKind kind = MeasureKind::WireBytes;
  std::uint64_t attributed_total = 0;
  std::uint64_t unknown_contributions = 0;
  std::uint64_t contribution_count = 0;
  std::optional<std::uint64_t> observed_total;
  std::uint64_t total_observations = 0;
  // signed difference: attributed - observed. Only meaningful when observed.
  std::int64_t delta = 0;
  std::uint64_t residual_to_unknown = 0;
  ConservationStatus status = ConservationStatus::Unverifiable;
  std::vector<EvidenceId> total_evidence;

  friend bool operator<(const DomainConservation& a, const DomainConservation& b) noexcept {
    if (a.scope != b.scope) return a.scope < b.scope;
    if (a.generation != b.generation) return a.generation < b.generation;
    return a.kind < b.kind;
  }
};

// The immutable record produced by closing a period. A correction produces a new
// revision whose parent digest points at the revision it supersedes, so the
// full lineage stays auditable.
struct PeriodRevision {
  PeriodId period;
  RevisionOrdinal revision;
  std::optional<Digest256> parent_digest;
  std::optional<CorrectionId> correction;
  std::string correction_reason;
  TimePoint closed_at;
  Digest256 content_digest;
  PolicyRevisionId policy_revision;
  TopologyRevisionId topology_revision;
  StoreId store;

  std::vector<GenerationId> generations;
  std::vector<Claim> claims;
  std::vector<UnknownEntry> unknowns;
  std::vector<ConflictRecord> conflicts;
  std::vector<RejectedObservation> rejected;
  std::vector<DomainConservation> conservation;
  std::vector<EfficiencySummary> efficiency;
  std::vector<ProvenanceClass> proof_surfaces;

  std::uint64_t evidence_considered = 0;
  std::uint64_t evidence_admitted = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t duplicate_suppressed = 0;
  bool stale_included = false;
  bool has_residual = false;
  bool has_excess = false;
  bool has_conflicts = false;
  bool has_unknown = false;
  bool complete = false;  // every domain had an independent total

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] Digest256 recompute_digest() const;
  [[nodiscard]] Result<void> verify_digest() const;
};

struct CorrectionRecord {
  CorrectionId id;
  PeriodId period;
  RevisionOrdinal from_revision;
  RevisionOrdinal to_revision;
  TimePoint created_at;
  std::string reason;
  std::string operator_label;
  Digest256 parent_digest;
  Digest256 new_digest;
  std::vector<EvidenceId> added_evidence;
  std::optional<Digest256> previous_correction;

  [[nodiscard]] std::string canonical_form() const;

  friend bool operator<(const CorrectionRecord& a, const CorrectionRecord& b) noexcept {
    if (a.period != b.period) return a.period < b.period;
    return a.to_revision < b.to_revision;
  }
};

}  // namespace fel
