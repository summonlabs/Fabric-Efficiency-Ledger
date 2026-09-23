// Fabric Efficiency Ledger - accounting cells, claims, and conflicts.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/id.hpp"
#include "fel/measure.hpp"
#include "fel/topology.hpp"

namespace fel {

// How an observation was correlated to a reporting cell. The basis is part of
// the cell identity: the same octets attributed through a path and through a
// flow are different cells and are never merged silently.
enum class AttributionBasis : std::uint8_t {
  ResourceDirect = 1,
  FlowDirect = 2,
  PathDirect = 3,
  ReservationDirect = 4,
  ScopeDeclared = 5,
  Unattributed = 6,
};

inline constexpr std::size_t kAttributionBasisCount = 6;

[[nodiscard]] std::string_view attribution_basis_name(AttributionBasis basis) noexcept;

// The identity of one accounting cell. Two claims with equal keys describe the
// same cell and must be merged; that is the mechanism that makes double counting
// detectable rather than merely unlikely.
struct ClaimKey {
  GenerationId generation;
  ScopeId scope;
  MeasureKind kind = MeasureKind::WireBytes;
  Category category = Category::UnknownUnattributed;
  AttributionBasis basis = AttributionBasis::Unattributed;
  ResourceId resource;
  FlowId flow;
  PathId path;
  ReservationId reservation;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] AccountingIdentity identity() const;

  friend bool operator==(const ClaimKey& a, const ClaimKey& b) noexcept;
  friend bool operator<(const ClaimKey& a, const ClaimKey& b) noexcept;
};

// A resolved conflict between sources about the same accounting cell.
struct ConflictRecord {
  AccountingIdentity identity;
  ClaimKey key;
  std::vector<EvidenceId> participants;
  std::vector<SourceId> sources;
  std::uint64_t distinct_amounts = 0;
  bool resolved = false;
  std::string resolution;
  std::string note;

  friend bool operator<(const ConflictRecord& a, const ConflictRecord& b) noexcept {
    return a.identity < b.identity;
  }
};

// A fused accounting cell: everything the ledger is willing to say about one
// tuple of (period, generation, scope, measure, category, basis, subject).
struct Claim {
  ClaimId id;
  AccountingIdentity identity;
  ClaimKey key;
  PeriodId period;
  AggregateCell cell;
  std::vector<EvidenceId> evidence;
  std::vector<SourceId> sources;
  std::vector<ProvenanceClass> provenance;
  std::vector<UnknownEntry> unknowns;
  bool has_conflict = false;
  bool stale_included = false;

  friend bool operator<(const Claim& a, const Claim& b) noexcept {
    if (a.key != b.key) return a.key < b.key;
    return a.id < b.id;
  }
  [[nodiscard]] std::string canonical_form() const;
};

// A quantity that could not be turned into a claim, retained so that nothing
// disappears without an explanation.
struct RejectedObservation {
  EvidenceId evidence;
  SourceId source;
  MeasureKind kind = MeasureKind::WireBytes;
  UnknownReason reason = UnknownReason::NoEvidence;
  std::string detail;

  friend bool operator<(const RejectedObservation& a, const RejectedObservation& b) noexcept {
    if (a.reason != b.reason) return a.reason < b.reason;
    if (a.source != b.source) return a.source < b.source;
    if (a.evidence != b.evidence) return a.evidence < b.evidence;
    return a.detail < b.detail;
  }
};

}  // namespace fel
