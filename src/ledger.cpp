// Fabric Efficiency Ledger - the accounting runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/ledger.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fel/checked.hpp"
#include "fel/freshness.hpp"
#include "fel/serialize.hpp"
#include "fel/version.hpp"
#include "ledger_json.hpp"
#include "topology_json.hpp"

namespace fel {
namespace {

using Window = std::pair<TimePoint, TimePoint>;

struct Contribution {
  ClaimKey key;
  std::uint64_t amount = 0;
  Window window;
  EvidenceId evidence;
  SourceId source;
  ProvenanceClass provenance = ProvenanceClass::Real;
  AuthorityRank authority;
  bool stale = false;
};

struct TotalCell {
  ScopeId scope;
  GenerationId generation;
  MeasureKind kind = MeasureKind::WireBytes;
  std::uint64_t amount = 0;
  Window window;
  EvidenceId evidence;
  SourceId source;
};

struct CellAccumulator {
  AggregateCell cell;
  std::vector<EvidenceId> evidence;
  std::vector<SourceId> sources;
  std::vector<ProvenanceClass> provenance;
  std::vector<UnknownEntry> unknowns;
  bool has_conflict = false;
  bool stale_included = false;
};

struct DomainKey {
  ScopeId scope;
  GenerationId generation;
  MeasureKind kind = MeasureKind::WireBytes;

  friend bool operator<(const DomainKey& a, const DomainKey& b) noexcept {
    if (a.scope != b.scope) return a.scope < b.scope;
    if (a.generation != b.generation) return a.generation < b.generation;
    return a.kind < b.kind;
  }
};

struct DomainAccumulator {
  std::uint64_t attributed_total = 0;
  std::uint64_t unknown_contributions = 0;
  std::uint64_t contribution_count = 0;
  std::optional<std::uint64_t> observed_total;
  std::uint64_t total_observations = 0;
  std::vector<EvidenceId> total_evidence;
};

// The complete result of the derivation pass. Everything downstream (claims,
// conservation, efficiency, aggregation, explanation) is a pure fold over this
// structure, which is what makes the whole pipeline order independent.
struct Derivation {
  std::vector<Contribution> contributions;
  std::vector<TotalCell> totals;
  std::vector<RejectedObservation> rejected;
  std::vector<UnknownEntry> unknowns;
  std::vector<ConflictRecord> conflicts;
  std::uint64_t considered = 0;
  std::uint64_t admitted = 0;
  std::uint64_t duplicate_suppressed = 0;
  std::uint64_t mixed_domain_rejections = 0;
  bool stale_included = false;
};

[[nodiscard]] bool window_within(const Window& window, const AccountingPeriod& period) {
  return window.first >= period.start && window.second <= period.end;
}

[[nodiscard]] std::uint64_t distinct_amounts(const std::map<std::uint64_t, std::set<SourceId>>& by_amount) {
  return static_cast<std::uint64_t>(by_amount.size());
}

void push_unknown(std::vector<UnknownEntry>& into, const UnknownEntry& entry) {
  if (std::find(into.begin(), into.end(), entry) == into.end()) {
    into.push_back(entry);
  }
}

struct ResolutionOutcome {
  bool resolved = false;
  std::uint64_t amount = 0;
  std::string description;
};

// Deterministic resolution of an amount dispute inside one accounting cell and
// reporting window. Never guesses: an unresolved dispute becomes unknown.
[[nodiscard]] Result<ResolutionOutcome> resolve_amount_dispute(
    const std::map<std::uint64_t, std::set<SourceId>>& by_amount,
    const std::map<SourceId, AuthorityRank>& authority, const LedgerPolicy& policy,
    const ClaimKey& key, std::string* note) {
  ResolutionOutcome outcome;
  if (by_amount.size() == 1) {
    outcome.resolved = true;
    outcome.amount = by_amount.begin()->first;
    outcome.description = "single agreed amount";
    return outcome;
  }

  if (policy.conflict == ConflictPolicy::StrictFail) {
    return make_error(ErrorCode::ConflictingEvidence,
                      "conflicting evidence for one accounting cell under a strict conflict policy",
                      key.canonical_form());
  }

  if (policy.conflict == ConflictPolicy::PreferHigherAuthority ||
      policy.tie_break != TieBreakPolicy::Reject) {
    AuthorityRank best;
    bool best_unique = false;
    SourceId best_source;
    for (const auto& entry : by_amount) {
      for (const auto& source : entry.second) {
        const auto found = authority.find(source);
        const AuthorityRank rank = found == authority.end() ? AuthorityRank{} : found->second;
        if (!best_unique || rank > best) {
          best = rank;
          best_source = source;
          best_unique = true;
        } else if (rank == best && source != best_source) {
          // Two equally authoritative sources disagree.
          if (policy.tie_break == TieBreakPolicy::LowestSourceIdWins && source < best_source) {
            best_source = source;
          } else if (policy.tie_break == TieBreakPolicy::HighestSourceIdWins &&
                     source > best_source) {
            best_source = source;
          } else if (policy.tie_break == TieBreakPolicy::Reject) {
            *note = "equal authority disagreement is unresolved by policy";
            outcome.description = "unresolved: equal authority";
            return outcome;
          }
        }
      }
    }
    if (!best_unique) {
      outcome.description = "unresolved: no authority information";
      return outcome;
    }
    std::size_t matches = 0;
    std::uint64_t chosen = 0;
    for (const auto& entry : by_amount) {
      if (entry.second.find(best_source) != entry.second.end()) {
        ++matches;
        chosen = entry.first;
      }
    }
    if (matches != 1) {
      *note = "the preferred source itself reports more than one amount";
      outcome.description = "unresolved: preferred source is self inconsistent";
      return outcome;
    }
    outcome.resolved = true;
    outcome.amount = chosen;
    outcome.description = "resolved by source authority";
    return outcome;
  }

  outcome.description = "unresolved: conflicting evidence";
  return outcome;
}

[[nodiscard]] Result<ClaimKey> attribute_observation(const Observation& observation,
                                                     const FabricTopology& topology) {
  ClaimKey key;
  key.generation = observation.generation;
  key.kind = observation.kind;
  key.category = observation.category;

  if (observation.resource.has_value()) {
    const Resource* resource = topology.find_resource(*observation.resource);
    if (resource == nullptr) {
      return make_error(ErrorCode::UnknownResource, "evidence names an undefined resource",
                        observation.resource->to_string());
    }
    const Scope* scope = topology.find_scope(resource->scope);
    if (scope == nullptr) {
      return make_error(ErrorCode::UnknownScope, "resource is not attached to a known scope",
                        resource->scope.to_string());
    }
    key.basis = AttributionBasis::ResourceDirect;
    key.resource = *observation.resource;
    key.scope = resource->scope;
    return key;
  }
  if (observation.path.has_value()) {
    const Path* path = topology.find_path(*observation.path);
    if (path == nullptr) {
      return make_error(ErrorCode::UnknownResource, "evidence names an undefined path",
                        observation.path->to_string());
    }
    key.basis = AttributionBasis::PathDirect;
    key.path = *observation.path;
    key.scope = path->scope;
    return key;
  }
  if (observation.flow.has_value()) {
    const Flow* flow = topology.find_flow(*observation.flow);
    if (flow == nullptr) {
      return make_error(ErrorCode::UnknownResource, "evidence names an undefined flow",
                        observation.flow->to_string());
    }
    key.basis = AttributionBasis::FlowDirect;
    key.flow = *observation.flow;
    key.scope = flow->scope;
    return key;
  }
  if (observation.reservation.has_value()) {
    const Reservation* reservation = topology.find_reservation(*observation.reservation);
    if (reservation == nullptr) {
      return make_error(ErrorCode::UnknownResource, "evidence names an undefined reservation",
                        observation.reservation->to_string());
    }
    key.basis = AttributionBasis::ReservationDirect;
    key.reservation = *observation.reservation;
    key.scope = reservation->scope;
    return key;
  }
  key.basis = AttributionBasis::Unattributed;
  return key;
}

}  // namespace
namespace {

constexpr std::size_t kMaxClaimEvidence = limits::kMaxExplanationEntries;

// ---------------------------------------------------------------------------
// Derivation
//
// A pure function of (policy, topology, period, retained evidence, as_of).
// Evidence order, arrival order, and thread scheduling cannot influence the
// result: every grouping structure is ordered and every fence is computed from
// the whole set rather than from the first record seen.
// ---------------------------------------------------------------------------
[[nodiscard]] Result<Derivation> run_derivation(const LedgerPolicy& policy,
                                                const FabricTopology& topology,
                                                const AccountingPeriod& period,
                                                const std::vector<Observation>& observations,
                                                TimePoint as_of) {
  Derivation result;
  std::vector<const Observation*> candidates;

  for (const auto& observation : observations) {
    if (!window_within(Window{observation.window_start, observation.window_end}, period)) {
      continue;
    }
    candidates.push_back(&observation);
  }
  result.considered = candidates.size();

  // Deterministic candidate order regardless of the order evidence was stored.
  std::sort(candidates.begin(), candidates.end(),
            [](const Observation* a, const Observation* b) { return a->id < b->id; });

  const auto reject = [&result](const Observation& observation, UnknownReason reason,
                                std::string detail) {
    RejectedObservation entry;
    entry.evidence = observation.id;
    entry.source = observation.source;
    entry.kind = observation.kind;
    entry.reason = reason;
    entry.detail = std::move(detail);
    result.rejected.push_back(std::move(entry));
    UnknownEntry unknown;
    unknown.scope = ScopeId{};
    unknown.generation = observation.generation;
    unknown.kind = observation.kind;
    unknown.reason = reason;
    unknown.evidence_count = 1;
    push_unknown(result.unknowns, unknown);
  };

  // ---- structural validation and ordinal numbering ------------------------
  std::set<EvidenceId> excluded;
  std::map<EvidenceId, std::vector<const Observation*>> by_id;
  for (const Observation* observation : candidates) {
    by_id[observation->id].push_back(observation);
  }
  for (const auto& entry : by_id) {
    if (entry.second.size() <= 1) {
      continue;
    }
    // Two records claiming the same evidence identity is a producer defect:
    // nothing can be counted, because we cannot tell which one is real.
    for (const Observation* observation : entry.second) {
      excluded.insert(observation->id);
      reject(*observation, UnknownReason::ConflictingEvidence,
             "two distinct records claim the same evidence identity");
    }
  }

  // ---- group 1: delivery origin ------------------------------------------
  std::map<OriginId, std::vector<const Observation*>> by_origin;
  std::map<std::tuple<SourceId, IncarnationId, std::uint64_t>, std::vector<const Observation*>>
      by_sequence;
  for (const Observation* observation : candidates) {
    by_origin[observation->origin].push_back(observation);
    by_sequence[{observation->source, observation->incarnation, observation->sequence.value()}]
        .push_back(observation);
  }

  const auto exclude_group = [&](const std::vector<const Observation*>& group, bool duplicate) {
    if (group.size() <= 1) {
      return;
    }
    const Digest256 reference = group.front()->content_digest();
    bool identical = true;
    for (const Observation* observation : group) {
      if (observation->content_digest() != reference) {
        identical = false;
        break;
      }
    }
    if (identical) {
      // The same physical measurement delivered more than once. Counting it
      // once is exactly the difference between accounting and double counting.
      for (std::size_t i = 1; i < group.size(); ++i) {
        excluded.insert(group[i]->id);
        result.duplicate_suppressed += 1;
        reject(*group[i], UnknownReason::DuplicateSuppressed,
               "duplicate delivery of the same evidence");
      }
      return;
    }
    ConflictRecord conflict;
    conflict.key.generation = group.front()->generation;
    conflict.key.kind = group.front()->kind;
    conflict.key.category = group.front()->category;
    conflict.note = duplicate ? "a source sequence was reused with different content"
                              : "the same delivery origin carries different content";
    conflict.resolution = "excluded: identical origin with divergent content";
    for (const Observation* observation : group) {
      excluded.insert(observation->id);
      conflict.participants.push_back(observation->id);
      conflict.sources.push_back(observation->source);
      reject(*observation, UnknownReason::DuplicateSuppressed, conflict.note);
    }
    std::sort(conflict.participants.begin(), conflict.participants.end());
    std::sort(conflict.sources.begin(), conflict.sources.end());
    conflict.sources.erase(std::unique(conflict.sources.begin(), conflict.sources.end()),
                           conflict.sources.end());
    conflict.distinct_amounts = 0;
    conflict.identity = conflict.key.identity();
    result.conflicts.push_back(std::move(conflict));
  };

  for (const auto& entry : by_origin) {
    exclude_group(entry.second, false);
  }
  for (const auto& entry : by_sequence) {
    exclude_group(entry.second, true);
  }

  // ---- group 2: epoch fencing per source incarnation ----------------------
  std::map<IncarnationId, std::uint64_t> peak_epoch;
  for (const Observation* observation : candidates) {
    auto found = peak_epoch.find(observation->incarnation);
    if (found == peak_epoch.end() || observation->epoch.value() > found->second) {
      peak_epoch[observation->incarnation] = observation->epoch.value();
    }
  }

  std::map<SourceId, AuthorityRank> authority;
  for (const auto& entry : topology.sources()) {
    authority.emplace(entry.first, entry.second.authority);
  }

  // ---- admissibility ------------------------------------------------------
  std::vector<const Observation*> admitted;
  for (const Observation* observation : candidates) {
    if (excluded.find(observation->id) != excluded.end()) {
      continue;
    }
    const Source* source = topology.find_source(observation->source);
    if (source == nullptr) {
      reject(*observation, UnknownReason::AttributionFailure, "source is not declared");
      continue;
    }
    const SourceIncarnation* incarnation =
        topology.find_incarnation(observation->incarnation);
    if (!provenance_is_countable(source->provenance)) {
      reject(*observation, UnknownReason::UnsupportedProvenance,
             "source provenance class is explicitly unsupported");
      continue;
    }
    if (incarnation != nullptr && observation->epoch.value() < peak_epoch[observation->incarnation]) {
      reject(*observation, UnknownReason::FencedEpoch,
             "the source reported a later epoch for this incarnation; this record is superseded");
      continue;
    }
    const AdmissibilityAssessment assessment =
        assess_admissibility(*observation, as_of, policy, topology, *source, incarnation);
    if (!assessment.accepted) {
      reject(*observation, assessment.reason, assessment.detail);
      if (assessment.freshness.klass == FreshnessClass::Stale) {
        result.stale_included = policy.stale == StaleEvidencePolicy::IncludeMarked;
      }
      continue;
    }
    admitted.push_back(observation);
  }

  // ---- correlation --------------------------------------------------------
  struct RawEntry {
    const Observation* observation;
    ClaimKey key;
  };
  std::vector<RawEntry> entries;
  for (const Observation* observation : admitted) {
    if (observation->role == ObservationRole::Total) {
      result.totals.push_back(TotalCell{
          ScopeId{}, observation->generation, observation->kind, observation->amount,
          Window{observation->window_start, observation->window_end}, observation->id,
          observation->source});
      continue;
    }
    auto key = attribute_observation(*observation, topology);
    if (!key.has_value()) {
      reject(*observation, UnknownReason::AttributionFailure, key.error().message);
      continue;
    }
    entries.push_back(RawEntry{observation, key.value()});
  }

  // Total observations must be attributed to a reporting scope as well; the
  // scope is derived from the same rule so that conservation domains line up.
  {
    std::vector<TotalCell> rebound;
    rebound.reserve(result.totals.size());
    for (auto& cell : result.totals) {
      const auto found = by_id.find(cell.evidence);
      const Observation* source_observation =
          found == by_id.end() || found->second.empty() ? nullptr : found->second.front();
      if (source_observation != nullptr) {
        auto key = attribute_observation(*source_observation, topology);
        if (key.has_value()) {
          cell.scope = key.value().scope;
        }
      }
      rebound.push_back(cell);
    }
    result.totals = std::move(rebound);
  }

  // ---- dispute resolution per (cell, window) ------------------------------
  std::map<std::pair<ClaimKey, Window>, std::vector<const Observation*>> groups;
  for (const auto& entry : entries) {
    groups[{entry.key, Window{entry.observation->window_start, entry.observation->window_end}}]
        .push_back(entry.observation);
  }

  for (const auto& entry : groups) {
    const ClaimKey& key = entry.first.first;
    const Window& window = entry.first.second;
    std::map<std::uint64_t, std::set<SourceId>> by_amount;
    for (const Observation* observation : entry.second) {
      by_amount[observation->amount].insert(observation->source);
    }
    if (by_amount.size() == 1) {
      const std::uint64_t amount = by_amount.begin()->first;
      // Deterministic single representative: the lowest evidence identity.
      const Observation* representative = *std::min_element(
          entry.second.begin(), entry.second.end(),
          [](const Observation* a, const Observation* b) { return a->id < b->id; });
      for (const Observation* observation : entry.second) {
        if (observation == representative) {
          continue;
        }
        // Every suppressed record is still explained, never silently dropped.
        result.duplicate_suppressed += 1;
        reject(*observation, UnknownReason::DuplicateSuppressed,
               "an equivalent amount for the same cell and window was already counted");
      }
      Contribution contribution;
      contribution.key = key;
      contribution.amount = amount;
      contribution.window = window;
      contribution.evidence = representative->id;
      contribution.source = representative->source;
      contribution.provenance =
          topology.find_source(representative->source) != nullptr
              ? topology.find_source(representative->source)->provenance
              : ProvenanceClass::Real;
      const auto rank = authority.find(representative->source);
      contribution.authority = rank == authority.end() ? AuthorityRank{} : rank->second;
      const AdmissibilityAssessment assessment = assess_admissibility(
          *representative, as_of, policy, topology, *topology.find_source(representative->source),
          topology.find_incarnation(representative->incarnation));
      contribution.stale = assessment.freshness.klass == FreshnessClass::Stale;
      result.contributions.push_back(std::move(contribution));
      continue;
    }

    std::string note;
    ResolutionOutcome outcome;
    FEL_TRY_ASSIGN(outcome, resolve_amount_dispute(by_amount, authority, policy, key, &note));
    ConflictRecord conflict;
    conflict.key = key;
    conflict.identity = key.identity();
    conflict.distinct_amounts = distinct_amounts(by_amount);
    conflict.resolved = outcome.resolved;
    conflict.resolution = outcome.description;
    conflict.note = note;
    for (const Observation* observation : entry.second) {
      conflict.participants.push_back(observation->id);
      conflict.sources.push_back(observation->source);
    }
    std::sort(conflict.participants.begin(), conflict.participants.end());
    std::sort(conflict.sources.begin(), conflict.sources.end());
    conflict.sources.erase(std::unique(conflict.sources.begin(), conflict.sources.end()),
                           conflict.sources.end());
    result.conflicts.push_back(std::move(conflict));

    if (outcome.resolved) {
      const Observation* chosen = nullptr;
      for (const Observation* observation : entry.second) {
        if (observation->amount == outcome.amount) {
          if (chosen == nullptr || observation->id < chosen->id) {
            chosen = observation;
          }
        }
      }
      Contribution contribution;
      contribution.key = key;
      contribution.amount = outcome.amount;
      contribution.window = window;
      contribution.evidence = chosen->id;
      contribution.source = chosen->source;
      const Source* source = topology.find_source(chosen->source);
      contribution.provenance = source != nullptr ? source->provenance : ProvenanceClass::Real;
      const auto rank = authority.find(chosen->source);
      contribution.authority = rank == authority.end() ? AuthorityRank{} : rank->second;
      result.contributions.push_back(std::move(contribution));
    } else {
      for (const Observation* observation : entry.second) {
        reject(*observation, UnknownReason::ConflictingEvidence,
               "sources disagree about this accounting cell");
      }
    }
  }

  {
    std::vector<TotalCell> kept;
    std::map<std::pair<DomainKey, Window>, std::vector<TotalCell>> total_groups;
    for (const auto& cell : result.totals) {
      total_groups[{DomainKey{cell.scope, cell.generation, cell.kind}, cell.window}].push_back(cell);
    }
    for (const auto& entry : total_groups) {
      std::vector<TotalCell> group = entry.second;
      std::sort(group.begin(), group.end(),
                [](const TotalCell& a, const TotalCell& b) { return a.evidence < b.evidence; });
      std::map<std::uint64_t, std::set<SourceId>> by_amount;
      for (const auto& cell : group) {
        by_amount[cell.amount].insert(cell.source);
      }
      const TotalCell representative = group.front();
      if (by_amount.size() != 1) {
        ConflictRecord conflict;
        conflict.key.generation = representative.generation;
        conflict.key.scope = representative.scope;
        conflict.key.kind = representative.kind;
        conflict.note = "independent totals disagree for a conservation domain";
        conflict.resolution = "unresolved: the domain is reported as unverifiable";
        conflict.distinct_amounts = distinct_amounts(by_amount);
        for (const auto& cell : entry.second) {
          conflict.participants.push_back(cell.evidence);
          conflict.sources.push_back(cell.source);
        }
        std::sort(conflict.sources.begin(), conflict.sources.end());
        conflict.sources.erase(std::unique(conflict.sources.begin(), conflict.sources.end()),
                               conflict.sources.end());
        conflict.identity = conflict.key.identity();
        result.conflicts.push_back(std::move(conflict));
        UnknownEntry unknown;
        unknown.scope = representative.scope;
        unknown.generation = representative.generation;
        unknown.kind = representative.kind;
        unknown.reason = UnknownReason::ConflictingEvidence;
        unknown.evidence_count = entry.second.size();
        push_unknown(result.unknowns, unknown);
        continue;
      }
      kept.push_back(representative);
    }
    result.totals = std::move(kept);
  }

  std::sort(result.contributions.begin(), result.contributions.end(),
            [](const Contribution& a, const Contribution& b) {
              if (a.key != b.key) return a.key < b.key;
              if (a.window != b.window) return a.window < b.window;
              return a.evidence < b.evidence;
            });
  std::sort(result.conflicts.begin(), result.conflicts.end());
  std::sort(result.unknowns.begin(), result.unknowns.end());
  std::sort(result.rejected.begin(), result.rejected.end());
  result.admitted = result.contributions.size();
  return result;
}

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// Folding
// ---------------------------------------------------------------------------
[[nodiscard]] Result<std::vector<Claim>> fold_claims(const Derivation& derivation,
                                                     const AccountingPeriod& period,
                                                     const std::vector<ConflictRecord>& conflicts) {
  std::map<ClaimKey, CellAccumulator> cells;
  for (const auto& contribution : derivation.contributions) {
    CellAccumulator& accumulator = cells[contribution.key];
    accumulator.cell.kind = contribution.key.kind;
    FEL_TRY(accumulator.cell.add_known(contribution.amount));
    if (accumulator.evidence.size() < kMaxClaimEvidence) {
      accumulator.evidence.push_back(contribution.evidence);
    }
    if (std::find(accumulator.sources.begin(), accumulator.sources.end(), contribution.source) ==
        accumulator.sources.end()) {
      accumulator.sources.push_back(contribution.source);
    }
    if (std::find(accumulator.provenance.begin(), accumulator.provenance.end(),
                  contribution.provenance) == accumulator.provenance.end()) {
      accumulator.provenance.push_back(contribution.provenance);
    }
    if (contribution.stale) {
      accumulator.stale_included = true;
    }
  }

  std::vector<Claim> claims;
  claims.reserve(cells.size());
  for (auto& entry : cells) {
    Claim claim;
    claim.key = entry.first;
    claim.identity = entry.first.identity();
    claim.period = period.id;
    claim.cell = entry.second.cell;
    claim.evidence = entry.second.evidence;
    std::sort(claim.evidence.begin(), claim.evidence.end());
    claim.sources = entry.second.sources;
    std::sort(claim.sources.begin(), claim.sources.end());
    claim.provenance = entry.second.provenance;
    std::sort(claim.provenance.begin(), claim.provenance.end());
    claim.stale_included = entry.second.stale_included;
    for (const auto& conflict : conflicts) {
      if (conflict.identity == claim.identity) {
        claim.has_conflict = true;
        break;
      }
    }
    claim.id = ClaimId::derive(
        {period.id.to_string(), claim.key.canonical_form(), claim.identity.to_string()});
    claims.push_back(std::move(claim));
  }
  std::sort(claims.begin(), claims.end());
  return claims;
}

[[nodiscard]] Result<std::vector<DomainConservation>> fold_conservation(
    const Derivation& derivation, const LedgerPolicy& policy,
    std::vector<UnknownEntry>* unknowns, bool* has_residual, bool* has_excess, bool* complete) {
  std::map<DomainKey, DomainAccumulator> domains;
  for (const auto& contribution : derivation.contributions) {
    DomainAccumulator& accumulator =
        domains[DomainKey{contribution.key.scope, contribution.key.generation,
                          contribution.key.kind}];
    const auto total = add_checked(accumulator.attributed_total, contribution.amount);
    if (!total.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow,
                        "conservation domain total overflowed while accumulating");
    }
    accumulator.attributed_total = *total;
    accumulator.contribution_count += 1;
  }
  for (const auto& entry : derivation.unknowns) {
    DomainAccumulator& accumulator =
        domains[DomainKey{entry.scope, entry.generation, entry.kind}];
    const auto total = add_checked(accumulator.unknown_contributions, entry.evidence_count);
    if (!total.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow,
                        "conservation domain unknown count overflowed");
    }
    accumulator.unknown_contributions = *total;
  }
  for (const auto& cell : derivation.totals) {
    DomainAccumulator& accumulator =
        domains[DomainKey{cell.scope, cell.generation, cell.kind}];
    if (!accumulator.observed_total.has_value()) {
      accumulator.observed_total = 0;
    }
    const auto total = add_checked(*accumulator.observed_total, cell.amount);
    if (!total.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow, "observed total overflowed");
    }
    accumulator.observed_total = *total;
    accumulator.total_observations += 1;
    accumulator.total_evidence.push_back(cell.evidence);
  }

  std::vector<DomainConservation> result;
  result.reserve(domains.size());
  bool all_complete = true;
  for (auto& entry : domains) {
    DomainConservation domain;
    domain.scope = entry.first.scope;
    domain.generation = entry.first.generation;
    domain.kind = entry.first.kind;
    domain.attributed_total = entry.second.attributed_total;
    domain.unknown_contributions = entry.second.unknown_contributions;
    domain.contribution_count = entry.second.contribution_count;
    domain.observed_total = entry.second.observed_total;
    domain.total_observations = entry.second.total_observations;
    domain.total_evidence = entry.second.total_evidence;
    std::sort(domain.total_evidence.begin(), domain.total_evidence.end());

    if (!domain.observed_total.has_value()) {
      domain.status = ConservationStatus::Unverifiable;
      all_complete = false;
      if (policy.require_total_observations) {
        UnknownEntry unknown;
        unknown.scope = domain.scope;
        unknown.generation = domain.generation;
        unknown.kind = domain.kind;
        unknown.reason = UnknownReason::NoEvidence;
        unknown.evidence_count = 1;
        unknown.note = "no independent total was reported for this conservation domain";
        push_unknown(*unknowns, unknown);
      }
    } else if (domain.attributed_total == *domain.observed_total) {
      domain.status = ConservationStatus::Consistent;
      domain.delta = 0;
    } else if (domain.attributed_total < *domain.observed_total) {
      const std::uint64_t residual = *domain.observed_total - domain.attributed_total;
      domain.status = ConservationStatus::Residual;
      domain.residual_to_unknown = residual;
      domain.delta = residual > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                         ? std::numeric_limits<std::int64_t>::min()
                         : -static_cast<std::int64_t>(residual);
      *has_residual = true;
      if (policy.residual == ResidualPolicy::StrictReject) {
        return make_error(ErrorCode::ConservationViolation,
                          "attributed categories do not account for the observed total",
                          "residual " + std::to_string(residual));
      }
      UnknownEntry unknown;
      unknown.scope = domain.scope;
      unknown.generation = domain.generation;
      unknown.kind = domain.kind;
      unknown.reason = UnknownReason::ResidualUnclassified;
      unknown.evidence_count = 1;
      unknown.note = "residual of " + std::to_string(residual) +
                     " " + std::string(measure_kind_unit(domain.kind)) +
                     " is not attributed to any classification";
      push_unknown(*unknowns, unknown);
    } else {
      const std::uint64_t excess = domain.attributed_total - *domain.observed_total;
      domain.status = ConservationStatus::Excess;
      domain.delta = excess > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                         ? std::numeric_limits<std::int64_t>::max()
                         : static_cast<std::int64_t>(excess);
      *has_excess = true;
      if (policy.excess == ExcessPolicy::StrictReject) {
        return make_error(ErrorCode::OverAttribution,
                          "attributed categories exceed the observed total",
                          "excess " + std::to_string(excess));
      }
      UnknownEntry unknown;
      unknown.scope = domain.scope;
      unknown.generation = domain.generation;
      unknown.kind = domain.kind;
      unknown.reason = UnknownReason::OverAttribution;
      unknown.evidence_count = 1;
      unknown.note = "attributed total exceeds the observed total by " +
                     std::to_string(excess) + " " +
                     std::string(measure_kind_unit(domain.kind)) +
                     "; the period is flagged as not conserved";
      push_unknown(*unknowns, unknown);
    }
    result.push_back(std::move(domain));
  }
  std::sort(result.begin(), result.end());
  *complete = all_complete && !*has_residual && !*has_excess;
  return result;
}

[[nodiscard]] std::vector<EfficiencySummary> fold_efficiency(
    const std::vector<Claim>& claims, const std::vector<DomainConservation>& conservation) {
  std::map<MeasureKind, EfficiencySummary> summaries;
  std::map<MeasureKind, bool> domain_ok;
  std::map<MeasureKind, bool> domain_complete;
  for (std::size_t i = 1; i <= kMeasureKindCount; ++i) {
    const auto kind = static_cast<MeasureKind>(i);
    EfficiencySummary summary;
    summary.kind = kind;
    summary.determinate = true;
    summaries.emplace(kind, summary);
    domain_ok.emplace(kind, true);
    domain_complete.emplace(kind, true);
  }

  for (const auto& claim : claims) {
    EfficiencySummary& summary = summaries[claim.cell.kind];
    const std::uint64_t amount = claim.cell.known_total;
    switch (usefulness_of(claim.key.category)) {
      case Usefulness::Useful:
        summary.useful += amount;
        break;
      case Usefulness::NecessaryOverhead:
        summary.necessary_overhead += amount;
        break;
      case Usefulness::Avoidable:
        summary.avoidable += amount;
        break;
      case Usefulness::Failed:
        summary.failed += amount;
        break;
      case Usefulness::Unknown:
        break;
    }
    summary.unknown_contributions += claim.cell.unknown_contributions;
    if (claim.key.category == Category::UnknownUnattributed) {
      summary.unknown_contributions += 1;
      summary.determinate = false;
      summary.indeterminacy_reason = "evidence exists that could not be classified";
    }
  }

  for (const auto& domain : conservation) {
    if (domain.status == ConservationStatus::Residual ||
        domain.status == ConservationStatus::Excess) {
      domain_ok[domain.kind] = false;
    }
    if (domain.status != ConservationStatus::Consistent) {
      domain_complete[domain.kind] = false;
    }
    // Unmeasured contributions reach the summary from two disjoint sets:
    // those folded into a claim, and those rejected before they could become
    // one. Neither set is a subset of the other, so both are counted, and a
    // non-zero result always makes the ratio indeterminate.
    const auto summary = summaries.find(domain.kind);
    if (summary != summaries.end()) {
      const auto next =
          add_checked(summary->second.unknown_contributions, domain.unknown_contributions);
      if (!next.has_value()) {
        summary->second.unknown_contributions = std::numeric_limits<std::uint64_t>::max();
        summary->second.determinate = false;
        summary->second.indeterminacy_reason = "the unmeasured contribution count overflowed";
      } else {
        summary->second.unknown_contributions = *next;
      }
    }
  }

  std::vector<EfficiencySummary> out;
  for (auto& entry : summaries) {
    EfficiencySummary& summary = entry.second;
    summary.denominator = summary.useful;
    const auto add = [&summary](std::uint64_t value) {
      const auto next = add_checked(summary.denominator, value);
      summary.denominator = next.has_value() ? *next : std::numeric_limits<std::uint64_t>::max();
    };
    add(summary.necessary_overhead);
    add(summary.avoidable);
    add(summary.failed);

    if (summary.unknown_contributions > 0) {
      summary.determinate = false;
      summary.indeterminacy_reason = "some contributions could not be measured";
    }
    if (!domain_ok[entry.first]) {
      summary.determinate = false;
      summary.indeterminacy_reason = "a conservation domain for this measure is not consistent";
    }
    if (summary.denominator == 0) {
      summary.coverage = Coverage::Unknown;
      if (summary.determinate) {
        summary.determinate = false;
        summary.indeterminacy_reason = "no known work was attributed to this measure";
      }
    } else if (!domain_complete[entry.first] || summary.unknown_contributions > 0) {
      summary.coverage = Coverage::Partial;
    } else {
      summary.coverage = Coverage::Known;
    }

    if (summary.determinate && summary.denominator > 0) {
      const std::uint64_t divisor = std::gcd(summary.useful, summary.denominator);
      summary.ratio_numerator = divisor == 0 ? summary.useful : summary.useful / divisor;
      summary.ratio_denominator = divisor == 0 ? summary.denominator : summary.denominator / divisor;
      const auto scaled = mul_checked(summary.useful, 1000);
      if (scaled.has_value()) {
        summary.ratio_permille = (*scaled + summary.denominator / 2) / summary.denominator;
      }
    }
    if (!summary.determinate && summary.indeterminacy_reason.empty()) {
      summary.indeterminacy_reason = "not determined";
    }
    out.push_back(summary);
  }
  return out;
}

[[nodiscard]] std::vector<ProvenanceClass> proof_surfaces_of(const Derivation& derivation) {
  std::set<ProvenanceClass> classes;
  for (const auto& contribution : derivation.contributions) {
    classes.insert(contribution.provenance);
  }
  return std::vector<ProvenanceClass>(classes.begin(), classes.end());
}

}  // namespace
// ---------------------------------------------------------------------------
// Result projections
// ---------------------------------------------------------------------------
bool operator<(const CellRow& a, const CellRow& b) noexcept {
  if (a.scope != b.scope) return a.scope < b.scope;
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.kind != b.kind) return a.kind < b.kind;
  if (a.category != b.category) return a.category < b.category;
  if (a.basis != b.basis) return a.basis < b.basis;
  if (a.resource != b.resource) return a.resource < b.resource;
  if (a.flow != b.flow) return a.flow < b.flow;
  if (a.path != b.path) return a.path < b.path;
  return a.reservation < b.reservation;
}

std::string CellRow::canonical_form() const {
  FieldWriter writer;
  writer.field("cell-row/1");
  writer.field(scope.is_nil() ? std::string("-") : scope.to_string());
  writer.field(generation.to_string());
  writer.field(measure_kind_name(kind));
  writer.field(category_name(category));
  writer.field(attribution_basis_name(basis));
  writer.field(resource.is_nil() ? std::string("-") : resource.to_string());
  writer.field(flow.is_nil() ? std::string("-") : flow.to_string());
  writer.field(path.is_nil() ? std::string("-") : path.to_string());
  writer.field(reservation.is_nil() ? std::string("-") : reservation.to_string());
  writer.field_u64(cell.known_total);
  writer.field_u64(cell.known_contributions);
  writer.field_u64(cell.unknown_contributions);
  for (const auto& value : evidence) {
    writer.field(value.to_string());
  }
  return writer.text();
}

std::string_view export_format_name(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::JsonLines:
      return "jsonl";
    case ExportFormat::Csv:
      return "csv";
    case ExportFormat::CanonicalJson:
      return "canonical-json";
  }
  return "invalid";
}

std::optional<ExportFormat> export_format_from_name(std::string_view name) noexcept {
  if (name == "jsonl" || name == "json-lines") return ExportFormat::JsonLines;
  if (name == "csv") return ExportFormat::Csv;
  if (name == "canonical-json" || name == "json") return ExportFormat::CanonicalJson;
  return std::nullopt;
}

namespace {

[[nodiscard]] Json cell_row_to_json(const CellRow& row, bool include_evidence) {
  Json item = Json::object();
  item.set("scope", Json::string(row.scope.is_nil() ? std::string("-") : row.scope.to_string()));
  item.set("generation", Json::string(row.generation.to_string()));
  item.set("measure", Json::string(std::string(measure_kind_name(row.kind))));
  item.set("unit", Json::string(std::string(measure_kind_unit(row.kind))));
  item.set("category", Json::string(std::string(category_name(row.category))));
  item.set("usefulness",
           Json::string(std::string(usefulness_name(usefulness_of(row.category)))));
  item.set("basis", Json::string(std::string(attribution_basis_name(row.basis))));
  item.set("resource",
           Json::string(row.resource.is_nil() ? std::string("-") : row.resource.to_string()));
  item.set("flow", Json::string(row.flow.is_nil() ? std::string("-") : row.flow.to_string()));
  item.set("path", Json::string(row.path.is_nil() ? std::string("-") : row.path.to_string()));
  item.set("reservation", Json::string(row.reservation.is_nil() ? std::string("-")
                                                                : row.reservation.to_string()));
  item.set("coverage", Json::string(std::string(coverage_name(row.cell.coverage()))));
  item.set("known_total", Json::number(row.cell.known_total));
  item.set("known_contributions", Json::number(row.cell.known_contributions));
  item.set("unknown_contributions", Json::number(row.cell.unknown_contributions));
  if (include_evidence) {
    Json evidence = Json::array();
    for (const auto& value : row.evidence) {
      evidence.push(Json::string(value.to_string()));
    }
    item.set("evidence", std::move(evidence));
  }
  return item;
}

[[nodiscard]] Json stamp_to_json(const ProvenanceStamp& stamp) {
  Json provenance = Json::object();
  provenance.set("policy_revision", Json::string(stamp.policy_revision.to_string()));
  provenance.set("policy_digest", Json::string(stamp.policy_digest.to_hex()));
  provenance.set("topology_revision", Json::string(stamp.topology_revision.to_string()));
  provenance.set("topology_digest", Json::string(stamp.topology_digest.to_hex()));
  provenance.set("store", Json::string(stamp.store.to_string()));
  provenance.set("store_revision", Json::number(stamp.store_revision));
  provenance.set("as_of", Json::string(stamp.as_of.to_rfc3339_millis()));
  provenance.set("store_watermark", Json::string(stamp.store_watermark.to_rfc3339_millis()));
  provenance.set("proof_surfaces", Json::string(stamp.proof_surface_label()));
  provenance.set("freshness_rebased", Json::boolean(stamp.freshness_rebased));
  provenance.set("contains_stale", Json::boolean(stamp.contains_stale));
  provenance.set("contains_unknown", Json::boolean(stamp.contains_unknown));
  Json generations = Json::array();
  for (const auto& value : stamp.generations) {
    generations.push(Json::string(value.to_string()));
  }
  provenance.set("generations", std::move(generations));
  return provenance;
}

}  // namespace

std::string PeriodSummary::canonical_form() const {
  FieldWriter writer;
  writer.field("period-summary/1");
  writer.field(period.to_string());
  writer.field_u64(ordinal.value());
  writer.field(label);
  writer.field(period_state_name(state));
  writer.field_u64(revision.value());
  writer.field_i64(start.nanos());
  writer.field_i64(end.nanos());
  writer.field_i64(closed_at.nanos());
  writer.field(content_digest.to_hex());
  writer.field(parent_digest.has_value() ? parent_digest->to_hex() : std::string("-"));
  writer.field(stamp.canonical_form());
  for (const auto& row : rows) {
    writer.field(row.canonical_form());
  }
  for (const auto& row : rolled_up) {
    writer.field(row.canonical_form());
  }
  for (const auto& entry : unknowns) {
    writer.field(entry.scope.to_string());
    writer.field(entry.generation.to_string());
    writer.field(measure_kind_name(entry.kind));
    writer.field(unknown_reason_name(entry.reason));
    writer.field_u64(entry.evidence_count);
    writer.field(entry.note);
  }
  for (const auto& conflict : conflicts) {
    writer.field(conflict.identity.to_string());
    writer.field(conflict.resolution);
  }
  for (const auto& domain : conservation) {
    writer.field(domain.scope.to_string());
    writer.field(domain.generation.to_string());
    writer.field(measure_kind_name(domain.kind));
    writer.field_u64(domain.attributed_total);
    writer.field(conservation_status_name(domain.status));
  }
  for (const auto& summary : efficiency) {
    writer.field(measure_kind_name(summary.kind));
    writer.field_bool(summary.determinate);
    writer.field_u64(summary.ratio_permille);
    writer.field(summary.indeterminacy_reason);
  }
  writer.field_u64(truncation.total_available);
  writer.field_u64(truncation.returned);
  writer.field_bool(truncation.truncated);
  return writer.text();
}

std::string PeriodSummary::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.period-summary/v1"));
  root.set("period", Json::string(period.to_string()));
  root.set("ordinal", Json::number(ordinal.value()));
  root.set("label", Json::string(label));
  root.set("state", Json::string(std::string(period_state_name(state))));
  root.set("revision", Json::number(revision.value()));
  root.set("start", Json::string(start.to_rfc3339_millis()));
  root.set("end", Json::string(end.to_rfc3339_millis()));
  root.set("closed_at", Json::string(closed_at.to_rfc3339_millis()));
  root.set("content_digest", Json::string(content_digest.to_hex()));
  root.set("parent_digest",
           Json::string(parent_digest.has_value() ? parent_digest->to_hex() : std::string("-")));
  root.set("truncated", Json::boolean(truncation.truncated));
  root.set("rows_total", Json::number(truncation.total_available));
  root.set("rows_returned", Json::number(truncation.returned));
  root.set("provenance", stamp_to_json(stamp));

  Json row_array = Json::array();
  for (const auto& row : rows) {
    row_array.push(cell_row_to_json(row, true));
  }
  root.set("rows", std::move(row_array));
  Json rolled_array = Json::array();
  for (const auto& row : rolled_up) {
    rolled_array.push(cell_row_to_json(row, false));
  }
  root.set("rolled_up", std::move(rolled_array));

  Json unknown_array = Json::array();
  for (const auto& entry : unknowns) {
    Json item = Json::object();
    item.set("scope", Json::string(entry.scope.is_nil() ? std::string("-") : entry.scope.to_string()));
    item.set("generation", Json::string(entry.generation.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(entry.kind))));
    item.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
    item.set("evidence_count", Json::number(entry.evidence_count));
    if (!entry.note.empty()) {
      item.set("note", Json::string(entry.note));
    }
    unknown_array.push(std::move(item));
  }
  root.set("unknowns", std::move(unknown_array));

  Json conservation_array = Json::array();
  for (const auto& domain : conservation) {
    Json item = Json::object();
    item.set("scope", Json::string(domain.scope.is_nil() ? std::string("-")
                                                         : domain.scope.to_string()));
    item.set("generation", Json::string(domain.generation.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(domain.kind))));
    item.set("attributed_total", Json::number(domain.attributed_total));
    item.set("observed_total", Json::string(domain.observed_total.has_value()
                                                ? std::to_string(*domain.observed_total)
                                                : std::string("-")));
    item.set("delta", Json::number(static_cast<std::uint64_t>(domain.delta < 0 ? -domain.delta
                                                                              : domain.delta)));
    item.set("delta_sign", Json::string(domain.delta < 0 ? "-" : "+"));
    item.set("status", Json::string(std::string(conservation_status_name(domain.status))));
    conservation_array.push(std::move(item));
  }
  root.set("conservation", std::move(conservation_array));

  Json efficiency_array = Json::array();
  for (const auto& summary : efficiency) {
    Json item = Json::object();
    item.set("measure", Json::string(std::string(measure_kind_name(summary.kind))));
    item.set("coverage", Json::string(std::string(coverage_name(summary.coverage))));
    item.set("determinate", Json::boolean(summary.determinate));
    item.set("indeterminacy_reason", Json::string(summary.indeterminacy_reason));
    item.set("useful", Json::number(summary.useful));
    item.set("necessary_overhead", Json::number(summary.necessary_overhead));
    item.set("avoidable", Json::number(summary.avoidable));
    item.set("failed", Json::number(summary.failed));
    item.set("denominator", Json::number(summary.denominator));
    item.set("ratio", Json::string(summary.ratio_text()));
    item.set("ratio_permille", Json::number(summary.ratio_permille));
    efficiency_array.push(std::move(item));
  }
  root.set("efficiency", std::move(efficiency_array));
  return root.dump(2);
}

std::string ReconciliationReport::canonical_form() const {
  FieldWriter writer;
  writer.field("reconciliation/1");
  writer.field(period.to_string());
  writer.field_u64(revision.value());
  writer.field_u64(domains_consistent);
  writer.field_u64(domains_residual);
  writer.field_u64(domains_excess);
  writer.field_u64(domains_unverifiable);
  writer.field_u64(mixed_domain_rejections);
  writer.field_bool(fully_consistent);
  writer.field(stamp.canonical_form());
  for (const auto& domain : domains) {
    writer.field(domain.scope.to_string());
    writer.field(domain.generation.to_string());
    writer.field(measure_kind_name(domain.kind));
    writer.field_u64(domain.attributed_total);
    writer.field(domain.observed_total.has_value() ? std::to_string(*domain.observed_total)
                                                   : std::string("-"));
    writer.field_i64(domain.delta);
    writer.field(conservation_status_name(domain.status));
  }
  for (const auto& line : violations) {
    writer.field(line);
  }
  return writer.text();
}

std::string ReconciliationReport::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.reconciliation/v1"));
  root.set("period", Json::string(period.to_string()));
  root.set("revision", Json::number(revision.value()));
  root.set("fully_consistent", Json::boolean(fully_consistent));
  root.set("domains_consistent", Json::number(domains_consistent));
  root.set("domains_residual", Json::number(domains_residual));
  root.set("domains_excess", Json::number(domains_excess));
  root.set("domains_unverifiable", Json::number(domains_unverifiable));
  root.set("mixed_domain_rejections", Json::number(mixed_domain_rejections));
  root.set("digest", Json::string(digest.to_hex()));
  root.set("provenance", stamp_to_json(stamp));
  Json domain_array = Json::array();
  for (const auto& domain : domains) {
    Json item = Json::object();
    item.set("scope", Json::string(domain.scope.is_nil() ? std::string("-")
                                                         : domain.scope.to_string()));
    item.set("generation", Json::string(domain.generation.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(domain.kind))));
    item.set("unit", Json::string(std::string(measure_kind_unit(domain.kind))));
    item.set("attributed_total", Json::number(domain.attributed_total));
    item.set("observed_total", Json::string(domain.observed_total.has_value()
                                                ? std::to_string(*domain.observed_total)
                                                : std::string("-")));
    item.set("residual_to_unknown", Json::number(domain.residual_to_unknown));
    item.set("unknown_contributions", Json::number(domain.unknown_contributions));
    item.set("contribution_count", Json::number(domain.contribution_count));
    item.set("status", Json::string(std::string(conservation_status_name(domain.status))));
    domain_array.push(std::move(item));
  }
  root.set("domains", std::move(domain_array));
  Json violation_array = Json::array();
  for (const auto& line : violations) {
    violation_array.push(Json::string(line));
  }
  root.set("violations", std::move(violation_array));
  return root.dump(2);
}

std::string Explanation::canonical_form() const {
  FieldWriter writer;
  writer.field("explanation/1");
  writer.field(period.to_string());
  writer.field_u64(revision.value());
  writer.field_bool(found);
  writer.field(reason);
  writer.field(identity.to_string());
  writer.field(cell.canonical_form());
  writer.field(stamp.canonical_form());
  for (const auto& step : derivation) {
    writer.field(step.rule);
    writer.field(step.outcome);
    writer.field(step.detail);
  }
  for (const auto& entry : excluded) {
    writer.field(entry.evidence.to_string());
    writer.field(unknown_reason_name(entry.reason));
    writer.field(entry.detail);
  }
  for (const auto& conflict : conflicts) {
    writer.field(conflict.identity.to_string());
    writer.field(conflict.resolution);
  }
  for (const auto& unknown : unknowns) {
    writer.field(unknown.scope.to_string());
    writer.field(measure_kind_name(unknown.kind));
    writer.field(unknown_reason_name(unknown.reason));
    writer.field_u64(unknown.evidence_count);
    writer.field(unknown.note);
  }
  writer.field_u64(truncation.total_available);
  writer.field_u64(truncation.returned);
  writer.field_bool(truncation.truncated);
  return writer.text();
}

std::string Explanation::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.explanation/v1"));
  root.set("period", Json::string(period.to_string()));
  root.set("revision", Json::number(revision.value()));
  root.set("found", Json::boolean(found));
  root.set("reason", Json::string(reason));
  root.set("identity", Json::string(identity.to_string()));
  root.set("digest", Json::string(digest.to_hex()));
  root.set("provenance", stamp_to_json(stamp));
  if (found) {
    root.set("cell", cell_row_to_json(cell, true));
  }
  Json steps = Json::array();
  for (const auto& step : derivation) {
    Json item = Json::object();
    item.set("rule", Json::string(step.rule));
    item.set("outcome", Json::string(step.outcome));
    item.set("detail", Json::string(step.detail));
    steps.push(std::move(item));
  }
  root.set("derivation", std::move(steps));
  Json excluded_array = Json::array();
  for (const auto& entry : excluded) {
    Json item = Json::object();
    item.set("evidence", Json::string(entry.evidence.to_string()));
    item.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
    item.set("detail", Json::string(entry.detail));
    excluded_array.push(std::move(item));
  }
  root.set("excluded", std::move(excluded_array));
  Json unknown_array = Json::array();
  for (const auto& entry : unknowns) {
    Json item = Json::object();
    item.set("scope",
             Json::string(entry.scope.is_nil() ? std::string("-") : entry.scope.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(entry.kind))));
    item.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
    item.set("evidence_count", Json::number(entry.evidence_count));
    item.set("note", Json::string(entry.note));
    unknown_array.push(std::move(item));
  }
  root.set("unknowns", std::move(unknown_array));
  return root.dump(2);
}

std::string ExportResult::canonical_form() const {
  FieldWriter writer;
  writer.field("export/1");
  writer.field(period.to_string());
  writer.field_u64(revision.value());
  writer.field(export_format_name(format));
  writer.field_u64(rows);
  writer.field(payload_digest.to_hex());
  writer.field(stamp.canonical_form());
  return writer.text();
}

std::string IntegrityReport::canonical_form() const {
  FieldWriter writer;
  writer.field("integrity/1");
  writer.field_bool(ok);
  writer.field(store.canonical_form());
  writer.field_u64(periods_checked);
  writer.field_u64(revisions_checked);
  writer.field_u64(revisions_failed);
  writer.field_u64(corrections_checked);
  writer.field_u64(lineages_verified);
  writer.field_u64(lineage_breaks);
  writer.field_u64(observations_retained);
  writer.field_u64(lock_audit_reentrancy);
  writer.field_u64(lock_audit_order);
  for (const auto& line : problems) {
    writer.field(line);
  }
  return writer.text();
}

std::string IntegrityReport::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.integrity/v1"));
  root.set("ok", Json::boolean(ok));
  root.set("store_clean", Json::boolean(store.clean()));
  root.set("store", Json::string(store.canonical_form()));
  root.set("periods_checked", Json::number(periods_checked));
  root.set("revisions_checked", Json::number(revisions_checked));
  root.set("revisions_failed", Json::number(revisions_failed));
  root.set("corrections_checked", Json::number(corrections_checked));
  root.set("lineages_verified", Json::number(lineages_verified));
  root.set("lineage_breaks", Json::number(lineage_breaks));
  root.set("observations_retained", Json::number(observations_retained));
  root.set("lock_audit_reentrancy", Json::number(lock_audit_reentrancy));
  root.set("lock_audit_order", Json::number(lock_audit_order));
  root.set("checked_at", Json::string(checked_at.to_rfc3339_millis()));
  Json problem_array = Json::array();
  for (const auto& line : problems) {
    problem_array.push(Json::string(line));
  }
  root.set("problems", std::move(problem_array));
  return root.dump(2);
}

std::string LedgerStats::canonical_form() const {
  FieldWriter writer;
  writer.field("ledger-stats/1");
  writer.field_u64(periods);
  writer.field_u64(open_periods);
  writer.field_u64(closed_periods);
  writer.field_u64(superseded_periods);
  writer.field_u64(revisions);
  writer.field_u64(corrections);
  writer.field_u64(observations_retained);
  writer.field_u64(observations_dropped_duplicate);
  writer.field_u64(observations_rejected);
  writer.field_u64(conflicts);
  writer.field_u64(claims_materialised);
  writer.field_i64(watermark.nanos());
  writer.field_i64(as_of.nanos());
  writer.field_bool(freshness_rebased);
  writer.field_u64(pool.submitted);
  writer.field_u64(pool.completed);
  writer.field_u64(pool.failed);
  writer.field_u64(pool.cancelled);
  writer.field_u64(pool.rejected);
  writer.field_u64(pool.jobs_in_flight);
  return writer.text();
}

std::string LedgerStats::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.stats/v1"));
  root.set("periods", Json::number(periods));
  root.set("open_periods", Json::number(open_periods));
  root.set("closed_periods", Json::number(closed_periods));
  root.set("superseded_periods", Json::number(superseded_periods));
  root.set("revisions", Json::number(revisions));
  root.set("corrections", Json::number(corrections));
  root.set("observations_retained", Json::number(observations_retained));
  root.set("observations_dropped_duplicate", Json::number(observations_dropped_duplicate));
  root.set("observations_rejected", Json::number(observations_rejected));
  root.set("conflicts", Json::number(conflicts));
  root.set("claims_materialised", Json::number(claims_materialised));
  root.set("watermark", Json::string(watermark.to_rfc3339_millis()));
  root.set("as_of", Json::string(as_of.to_rfc3339_millis()));
  root.set("freshness_rebased", Json::boolean(freshness_rebased));
  Json pool_json = Json::object();
  pool_json.set("submitted", Json::number(pool.submitted));
  pool_json.set("completed", Json::number(pool.completed));
  pool_json.set("failed", Json::number(pool.failed));
  pool_json.set("cancelled", Json::number(pool.cancelled));
  pool_json.set("rejected", Json::number(pool.rejected));
  pool_json.set("jobs_in_flight", Json::number(pool.jobs_in_flight));
  pool_json.set("running", Json::boolean(pool.running));
  root.set("pool", std::move(pool_json));
  Json audit = Json::object();
  audit.set("acquisitions", Json::number(lock_audit.acquisitions));
  audit.set("reentrancy_detections", Json::number(lock_audit.reentrancy_detections));
  audit.set("lock_order_violations", Json::number(lock_audit.lock_order_violations));
  root.set("lock_audit", std::move(audit));
  return root.dump(2);
}

PeriodId derive_period_id(TimePoint start, TimePoint end) {
  return PeriodId::derive({"period", std::to_string(start.nanos()), std::to_string(end.nanos())});
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
struct Ledger::Impl {
  CheckedMutex state{LockRank::StoreState};
  std::map<PeriodId, AccountingPeriod> periods;
  std::map<PeriodId, std::vector<PeriodRevision>> revisions;
  std::map<PeriodId, std::vector<CorrectionRecord>> corrections;
  // A multimap: two records may claim the same evidence identity, and that
  // collision itself is evidence that neither can be trusted. Both are kept so
  // that the collision survives a restart and is decided by the derivation.
  std::multimap<EvidenceId, Observation> observations;
  std::vector<Digest256> policy_history;
  std::vector<Digest256> topology_history;
  std::set<PeriodId> folded_periods;
  std::set<PeriodId> correction_required;
  std::uint64_t duplicate_suppressed = 0;
  std::uint64_t rejected_total = 0;
  std::uint64_t conflicts_total = 0;
  std::uint64_t claims_materialised = 0;
  bool freshness_rebased = false;
  std::unique_ptr<ThreadPool> pool;
};

namespace {

// The policy snapshot is stored as JSON so that a store written by one release
// can be inspected, diffed, and re-read by another.
[[nodiscard]] Json policy_to_json(const LedgerPolicy& policy) {
  Json root = Json::object();
  root.set("format_version", Json::number(static_cast<std::uint64_t>(policy.format_version)));
  Json freshness = Json::object();
  freshness.set("fresh_ttl_ms",
                Json::number(static_cast<std::uint64_t>(policy.freshness.fresh_ttl.millis())));
  freshness.set("stale_ttl_ms",
                Json::number(static_cast<std::uint64_t>(policy.freshness.stale_ttl.millis())));
  freshness.set("clock_skew_allowance_ms",
                Json::number(static_cast<std::uint64_t>(
                    policy.freshness.clock_skew_allowance.millis())));
  freshness.set("require_generation_binding",
                Json::boolean(policy.freshness.require_generation_binding));
  freshness.set("require_incarnation_coverage",
                Json::boolean(policy.freshness.require_incarnation_coverage));
  freshness.set("reject_future_evidence",
                Json::boolean(policy.freshness.reject_future_evidence));
  root.set("freshness", std::move(freshness));
  root.set("conflict", Json::string(std::string(conflict_policy_name(policy.conflict))));
  root.set("residual", Json::string(std::string(residual_policy_name(policy.residual))));
  root.set("excess", Json::string(std::string(excess_policy_name(policy.excess))));
  root.set("stale", Json::string(std::string(stale_policy_name(policy.stale))));
  root.set("late_evidence", Json::string(std::string(late_policy_name(policy.late_evidence))));
  root.set("recovery", Json::string(std::string(recovery_policy_name(policy.recovery))));
  root.set("tie_break", Json::string(std::string(tie_break_policy_name(policy.tie_break))));
  root.set("require_total_observations", Json::boolean(policy.require_total_observations));
  root.set("max_result_rows", Json::number(static_cast<std::uint64_t>(policy.max_result_rows)));
  root.set("max_explanation_entries",
           Json::number(static_cast<std::uint64_t>(policy.max_explanation_entries)));
  root.set("max_conflict_records",
           Json::number(static_cast<std::uint64_t>(policy.max_conflict_records)));
  root.set("max_aggregation_buckets",
           Json::number(static_cast<std::uint64_t>(policy.max_aggregation_buckets)));
  root.set("max_period_span_ms",
           Json::number(static_cast<std::uint64_t>(policy.max_period_span.millis())));
  root.set("max_lookback_ms",
           Json::number(static_cast<std::uint64_t>(policy.max_lookback.millis())));
  root.set("workers", Json::number(static_cast<std::uint64_t>(policy.workers)));
  root.set("queue_depth", Json::number(static_cast<std::uint64_t>(policy.queue_depth)));
  root.set("max_batch_records",
           Json::number(static_cast<std::uint64_t>(policy.max_batch_records)));
  root.set("max_store_bytes", Json::number(policy.max_store_bytes));
  root.set("max_segment_bytes", Json::number(policy.max_segment_bytes));
  root.set("max_segments", Json::number(policy.max_segments));
  root.set("max_retained_observations", Json::number(policy.max_retained_observations));
  root.set("max_corrections_per_period",
           Json::number(static_cast<std::uint64_t>(policy.max_corrections_per_period)));
  return root;
}

}  // namespace

Ledger::Ledger() : impl_(std::make_unique<Impl>()) {}

Ledger::~Ledger() {
  if (impl_ != nullptr && impl_->pool != nullptr) {
    impl_->pool->shutdown(ShutdownMode::Abort);
  }
}

void Ledger::refresh_stamp() const {
  ProvenanceStamp stamp;
  stamp.policy_revision = policy_.revision_id();
  stamp.policy_digest = policy_.digest();
  stamp.topology_revision = topology_.revision_id();
  stamp.topology_digest = topology_.digest();
  stamp.store = store_ != nullptr
                    ? store_->store_id()
                    : StoreId::derive({"memory", policy_.digest().to_hex(),
                                       topology_.digest().to_hex()});
  stamp.store_revision = store_ != nullptr ? store_->revision() : 0;
  stamp.store_watermark = store_ != nullptr ? store_->watermark() : TimePoint{};
  stamp.freshness_rebased = impl_ != nullptr && impl_->freshness_rebased;
  stamp.as_of = stamp_.as_of.is_zero() ? stamp.store_watermark : stamp_.as_of;
  std::set<GenerationId> generations;
  for (const auto& entry : topology_.generations()) {
    generations.insert(entry.first);
  }
  stamp.generations.assign(generations.begin(), generations.end());
  stamp_ = std::move(stamp);
}

TimePoint Ledger::resolve_as_of(const std::optional<TimePoint>& requested) const {
  if (requested.has_value()) {
    return requested.value();
  }
  const TimePoint watermark = store_ != nullptr ? store_->watermark() : TimePoint{};
  if (watermark.is_zero()) {
    return SystemClock{}.now();
  }
  return watermark;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------
Result<std::unique_ptr<Ledger>> Ledger::open(const OpenOptions& options) {
  FEL_TRY(options.policy.validate());
  if (options.store_path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a ledger store path is required");
  }

  StoreConfig config;
  config.path = options.store_path;
  config.mode = options.mode;
  config.recovery = options.policy.recovery;
  config.max_bytes = options.policy.max_store_bytes;
  config.max_segment_bytes = options.policy.max_segment_bytes;
  config.max_segments = options.policy.max_segments;
  config.take_exclusive_lock = options.take_store_lock;
  config.lock_timeout_ms = options.lock_timeout_ms;
  config.lock_hold_max_ms = options.lock_hold_max_ms;

  auto store = LedgerStore::open(config);
  if (!store.has_value()) {
    return store.error();
  }

  std::unique_ptr<Ledger> ledger(new Ledger());
  ledger->policy_ = options.policy;
  ledger->topology_ = options.topology;
  ledger->store_ = std::move(store.value());
  ledger->impl_ = std::make_unique<Impl>();
  if (options.enable_worker_pool) {
    PoolConfig pool_config;
    pool_config.workers = options.policy.workers;
    pool_config.queue_depth = options.policy.queue_depth;
    pool_config.name = "fel-ingest";
    ledger->impl_->pool = std::make_unique<ThreadPool>(pool_config);
    FEL_TRY(ledger->impl_->pool->start());
  }

  std::optional<LedgerPolicy> stored_policy;
  std::optional<FabricTopology> stored_topology;

  const auto visitor = [&](const StoreRecord& record) {
    switch (record.kind) {
      case RecordKind::PolicySnapshot: {
        auto parsed = parse_policy(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        stored_policy = parsed.value();
        ledger->impl_->policy_history.push_back(parsed.value().digest());
        break;
      }
      case RecordKind::TopologySnapshot: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        auto restored = detail::topology_from_json(parsed.value());
        if (!restored.has_value()) {
          return;
        }
        stored_topology = restored.value();
        ledger->impl_->topology_history.push_back(restored.value().digest());
        break;
      }
      case RecordKind::Observation: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        auto observation = detail::observation_from_json(parsed.value());
        if (observation.has_value()) {
          ledger->impl_->observations.emplace(observation.value().id,
                                              std::move(observation.value()));
        }
        break;
      }
      case RecordKind::PeriodOpened: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        auto period = detail::period_from_json(parsed.value());
        if (period.has_value()) {
          ledger->impl_->periods[period.value().id] = std::move(period.value());
        }
        break;
      }
      case RecordKind::PeriodClosed: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        auto revision = detail::revision_from_json(parsed.value());
        if (!revision.has_value()) {
          return;
        }
        const PeriodId period = revision.value().period;
        auto& list = ledger->impl_->revisions[period];
        list.push_back(std::move(revision.value()));
        std::sort(list.begin(), list.end(), [](const PeriodRevision& a, const PeriodRevision& b) {
          return a.revision < b.revision;
        });
        auto found = ledger->impl_->periods.find(period);
        if (found != ledger->impl_->periods.end() &&
            found->second.current_revision < list.back().revision) {
          found->second.current_revision = list.back().revision;
          found->second.state =
              list.back().revision.value() > 1 ? PeriodState::Superseded : PeriodState::Closed;
        }
        break;
      }
      case RecordKind::Correction: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        auto correction = detail::correction_from_json(parsed.value());
        if (correction.has_value()) {
          ledger->impl_->corrections[correction.value().period].push_back(
              std::move(correction.value()));
        }
        break;
      }
      case RecordKind::Compaction: {
        auto parsed = Json::parse(record.payload);
        if (!parsed.has_value()) {
          return;
        }
        const Json* list = parsed.value().find("folded_periods");
        if (list == nullptr || !list->is_array()) {
          return;
        }
        for (const auto& item : list->as_array()) {
          if (!item.is_string()) {
            continue;
          }
          auto parsed_id = PeriodId::parse(item.as_string());
          if (parsed_id.has_value()) {
            ledger->impl_->folded_periods.insert(parsed_id.value());
          }
        }
        break;
      }
      case RecordKind::Watermark:
        break;
    }
  };
  FEL_TRY(ledger->store_->replay(visitor));

  if (stored_policy.has_value()) {
    const bool caller_default = ledger->policy_.digest() == LedgerPolicy{}.digest();
    if (ledger->policy_.digest() != stored_policy->digest()) {
      bool replay_of_older = false;
      const auto& history = ledger->impl_->policy_history;
      for (std::size_t i = 0; i + 1 < history.size(); ++i) {
        if (history[i] == ledger->policy_.digest()) {
          replay_of_older = true;
          break;
        }
      }
      if (replay_of_older) {
        return make_error(ErrorCode::PolicyRevisionMismatch,
                          "the supplied policy revision was superseded in this store");
      }
      if (!caller_default) {
        return make_error(ErrorCode::PolicyRevisionMismatch,
                          "the supplied policy differs from the policy recorded in the store");
      }
      ledger->policy_ = *stored_policy;
    }
  }
  if (stored_topology.has_value()) {
    if (ledger->topology_.size() == 0) {
      ledger->topology_ = *stored_topology;
    } else if (ledger->topology_.digest() != stored_topology->digest()) {
      const auto& history = ledger->impl_->topology_history;
      for (std::size_t i = 0; i + 1 < history.size(); ++i) {
        if (history[i] == ledger->topology_.digest()) {
          return make_error(ErrorCode::TopologyRevisionMismatch,
                            "the supplied topology revision was superseded in this store");
        }
      }
    }
  }

  FEL_TRY(ledger->topology_.validate());

  std::vector<std::pair<RecordKind, std::string>> bootstrap;
  if (!stored_policy.has_value() ||
      stored_policy->digest() != ledger->policy_.digest()) {
    bootstrap.emplace_back(RecordKind::PolicySnapshot, policy_to_json(ledger->policy_).dump(0));
  }
  if (!stored_topology.has_value() ||
      stored_topology->digest() != ledger->topology_.digest()) {
    bootstrap.emplace_back(RecordKind::TopologySnapshot,
                           detail::topology_to_json(ledger->topology_).dump(0));
  }
  if (!bootstrap.empty()) {
    FEL_TRY(ledger->store_->append_many(bootstrap, ledger->resolve_as_of(options.as_of)));
  }

  std::optional<TimePoint> requested = options.as_of;
  const TimePoint watermark = ledger->store_->watermark();
  if (!requested.has_value() && options.rebase_freshness_on_open && !watermark.is_zero()) {
    const TimePoint wall = SystemClock{}.now();
    requested = wall > watermark ? wall : watermark;
  }
  ledger->stamp_.as_of = ledger->resolve_as_of(requested);
  ledger->impl_->freshness_rebased = !watermark.is_zero() && ledger->stamp_.as_of > watermark;
  ledger->refresh_stamp();
  return std::unique_ptr<Ledger>(std::move(ledger));
}

Result<std::unique_ptr<Ledger>> Ledger::in_memory(LedgerPolicy policy, FabricTopology topology,
                                                 TimePoint as_of) {
  FEL_TRY(policy.validate());
  FEL_TRY(topology.validate());
  std::unique_ptr<Ledger> ledger(new Ledger());
  ledger->policy_ = std::move(policy);
  ledger->topology_ = std::move(topology);
  ledger->impl_ = std::make_unique<Impl>();
  ledger->stamp_.as_of = as_of;
  ledger->refresh_stamp();
  return std::unique_ptr<Ledger>(std::move(ledger));
}

// ---------------------------------------------------------------------------
// Periods
// ---------------------------------------------------------------------------
Result<PeriodId> Ledger::open_period(TimePoint start, TimePoint end, std::string label) {
  if (!(start < end)) {
    return make_error(ErrorCode::InvalidArgument, "period end must be after period start");
  }
  const Duration span = end - start;
  if (span.nanos() < limits::kMinWindowNanos || span.nanos() > policy_.max_period_span.nanos()) {
    return make_error(ErrorCode::InvalidArgument, "period span is outside the policy window");
  }
  if (label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "period label exceeds the label budget");
  }
  auto guard = impl_->state.acquire("ledger.open_period");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  if (impl_->periods.size() >= limits::kMaxPeriods) {
    return make_error(ErrorCode::CapacityExceeded, "period budget exhausted");
  }
  const PeriodId id = derive_period_id(start, end);
  if (impl_->periods.find(id) != impl_->periods.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "an identical period already exists",
                      id.to_string());
  }
  for (const auto& entry : impl_->periods) {
    const AccountingPeriod& other = entry.second;
    if (start < other.end && other.start < end) {
      return make_error(ErrorCode::DuplicateDefinition, "period overlaps an existing period",
                        entry.first.to_string());
    }
  }
  AccountingPeriod period;
  period.id = id;
  period.ordinal = Ordinal{static_cast<std::uint64_t>(impl_->periods.size()) + 1};
  period.label = std::move(label);
  period.start = start;
  period.end = end;
  period.state = PeriodState::Open;

  if (store_ != nullptr && !store_->read_only()) {
    FEL_TRY(store_->append(RecordKind::PeriodOpened, detail::period_to_json(period).dump(0),
                           stamp_.as_of));
  }
  impl_->periods.emplace(id, period);
  refresh_stamp();
  return id;
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] const AccountingPeriod* find_period_for(
    const std::map<PeriodId, AccountingPeriod>& periods, const Observation& observation) {
  const AccountingPeriod* best = nullptr;
  for (const auto& entry : periods) {
    const AccountingPeriod& period = entry.second;
    if (observation.window_start >= period.start && observation.window_end <= period.end) {
      if (best == nullptr || period.start < best->start) {
        best = &period;
      }
    }
  }
  return best;
}

}  // namespace

Result<IngestReport> Ledger::ingest(const ObservationBatch& batch, const ClaimOptions& options) {
  if (batch.observations.size() > policy_.max_batch_records) {
    return make_error(ErrorCode::TooManyItems, "evidence batch exceeds the policy record budget");
  }
  auto guard = impl_->state.acquire("ledger.ingest");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }

  IngestReport report;
  report.received = batch.observations.size();
  report.batch_digest = batch.document_digest;

  std::uint64_t incoming = 0;
  for (const auto& observation : batch.observations) {
    bool already_present = false;
    const auto range = impl_->observations.equal_range(observation.id);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second.record_digest() == observation.record_digest()) {
        already_present = true;
        break;
      }
    }
    if (!already_present) {
      ++incoming;
    }
  }
  const auto projected = add_checked(impl_->observations.size(), incoming);
  if (!projected.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "observation count overflowed");
  }
  if (*projected > policy_.max_retained_observations) {
    return make_error(ErrorCode::CapacityExceeded,
                      "retained evidence would exceed the policy budget; close and compact a period",
                      std::to_string(*projected));
  }

  std::vector<std::pair<RecordKind, std::string>> records;
  std::vector<Observation> accepted;
  accepted.reserve(batch.observations.size());

  for (const auto& observation : batch.observations) {
    const auto shape = observation.validate_shape();
    if (!shape.has_value()) {
      ++report.rejected_shape;
      report.diagnostics.push_back("evidence " + observation.id.to_string() + ": " +
                                   shape.error().message);
      continue;
    }
    const Source* source = topology_.find_source(observation.source);
    if (source == nullptr) {
      ++report.rejected_unknown_source;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": source is not declared");
      continue;
    }
    const SourceIncarnation* incarnation = topology_.find_incarnation(observation.incarnation);
    if (incarnation == nullptr) {
      ++report.rejected_retired_incarnation;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": source incarnation is not declared");
      continue;
    }
    if (incarnation->retired_at.has_value() && observation.received_at > *incarnation->retired_at) {
      ++report.rejected_retired_incarnation;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": the source incarnation was already retired");
      continue;
    }
    if (observation.epoch < incarnation->boot_epoch) {
      ++report.rejected_epoch_fenced;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": the epoch precedes the incarnation boot epoch");
      continue;
    }
    if (topology_.find_generation(observation.generation) == nullptr) {
      ++report.rejected_generation;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": the generation is not declared");
      continue;
    }
    if (!provenance_is_countable(source->provenance)) {
      ++report.rejected_unsupported_provenance;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": the source provenance class is unsupported");
      continue;
    }

    const AccountingPeriod* target = nullptr;
    if (options.period.has_value()) {
      const auto found = impl_->periods.find(options.period.value());
      if (found == impl_->periods.end()) {
        ++report.rejected_no_period;
        report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                     ": the requested period does not exist");
        continue;
      }
      target = &found->second;
      if (observation.window_start < target->start || observation.window_end > target->end) {
        ++report.rejected_no_period;
        report.diagnostics.push_back(
            "evidence " + observation.id.to_string() +
            ": the reporting window is not contained in the requested period");
        continue;
      }
    } else {
      target = find_period_for(impl_->periods, observation);
      if (target == nullptr) {
        ++report.rejected_no_period;
        report.diagnostics.push_back(
            "evidence " + observation.id.to_string() +
            ": no accounting period contains this reporting window; open one first");
        continue;
      }
    }

    if (target->state != PeriodState::Open) {
      ++report.late_evidence;
      if (policy_.late_evidence == LateEvidencePolicy::Reject) {
        report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                     ": the target period is closed and late evidence is refused");
        continue;
      }
      impl_->correction_required.insert(target->id);
    }

    bool benign_duplicate = false;
    bool identity_collision = false;
    const auto range = impl_->observations.equal_range(observation.id);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second.record_digest() == observation.record_digest()) {
        benign_duplicate = true;
        break;
      }
      identity_collision = true;
    }
    if (benign_duplicate) {
      ++report.duplicate_suppressed;
      continue;
    }
    if (identity_collision) {
      // The record is stored anyway: the collision must be visible to the
      // derivation and must survive a restart, so neither copy is counted.
      ++report.conflicts;
      report.diagnostics.push_back("evidence " + observation.id.to_string() +
                                   ": a different record already claims this evidence identity");
    }

    records.emplace_back(RecordKind::Observation,
                         detail::observation_to_json(observation).dump(0));
    accepted.push_back(observation);
  }

  if (store_ != nullptr && !store_->read_only() && !records.empty()) {
    FEL_TRY(store_->append_many(records, stamp_.as_of));
  }
  for (auto& observation : accepted) {
    const AccountingPeriod* target = find_period_for(impl_->periods, observation);
    const EvidenceId id = observation.id;
    impl_->observations.emplace(id, std::move(observation));
    ++report.accepted;
    if (target != nullptr) {
      auto found = impl_->periods.find(target->id);
      if (found != impl_->periods.end()) {
        found->second.observations_ingested += 1;
      }
    }
  }
  impl_->conflicts_total += report.conflicts;
  impl_->rejected_total += report.rejected_shape + report.rejected_unknown_source +
                           report.rejected_retired_incarnation + report.rejected_epoch_fenced +
                           report.rejected_sequence_fenced + report.rejected_generation +
                           report.rejected_unsupported_provenance + report.rejected_capacity +
                           report.rejected_no_period;
  refresh_stamp();
  return report;
}

Result<IngestReport> Ledger::ingest_parallel(const ObservationBatch& batch,
                                             const ClaimOptions& options) {
  if (impl_->pool == nullptr || batch.observations.size() < 64) {
    return ingest(batch, options);
  }
  // Only shape validation and JSON encoding are spread across workers. The
  // commit path is identical to the sequential path and the derivation pass is
  // order independent, so concurrency cannot change a single accounting total.
  std::vector<std::string> encoded(batch.observations.size());
  std::vector<char> valid(batch.observations.size(), 1);
  const std::size_t chunks = std::max<std::size_t>(1, policy_.workers);
  const std::size_t per_chunk = (batch.observations.size() + chunks - 1) / chunks;

  std::atomic<std::uint64_t> remaining{0};
  std::mutex done_mutex;
  std::condition_variable done_cv;

  for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
    const std::size_t begin = chunk * per_chunk;
    const std::size_t end = std::min(begin + per_chunk, batch.observations.size());
    if (begin >= end) {
      continue;
    }
    remaining.fetch_add(1, std::memory_order_acq_rel);
    const auto job = [&batch, &encoded, &valid, begin, end, &remaining, &done_mutex,
                      &done_cv](const CancellationToken& token) {
      for (std::size_t i = begin; i < end; ++i) {
        if (token.cancelled()) {
          break;
        }
        const auto& observation = batch.observations[i];
        if (!observation.validate_shape().has_value()) {
          valid[i] = 0;
          continue;
        }
        encoded[i] = detail::observation_to_json(observation).dump(0);
      }
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(done_mutex);
        done_cv.notify_all();
      }
    };
    const auto submitted = impl_->pool->submit(job);
    if (!submitted.has_value()) {
      remaining.fetch_sub(1, std::memory_order_acq_rel);
      break;
    }
  }
  {
    std::unique_lock<std::mutex> lock(done_mutex);
    done_cv.wait(lock, [&remaining] { return remaining.load(std::memory_order_acquire) == 0; });
  }
  return ingest(batch, options);
}

// ---------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------
Result<PeriodRevision> Ledger::derive_locked(const AccountingPeriod& period, TimePoint as_of,
                                             RevisionOrdinal revision,
                                             std::optional<Digest256> parent_digest,
                                             std::optional<CorrectionId> correction,
                                             std::string correction_reason,
                                             std::string operator_label) const {
  std::vector<Observation> evidence;
  evidence.reserve(impl_->observations.size());
  for (const auto& entry : impl_->observations) {
    evidence.push_back(entry.second);
  }

  FEL_TRY_ASSIGN(Derivation derivation,
                 run_derivation(policy_, topology_, period, evidence, as_of));

  std::vector<UnknownEntry> unknowns = derivation.unknowns;
  bool has_residual = false;
  bool has_excess = false;
  bool complete = false;
  FEL_TRY_ASSIGN(std::vector<DomainConservation> conservation,
                 fold_conservation(derivation, policy_, &unknowns, &has_residual, &has_excess,
                                   &complete));
  FEL_TRY_ASSIGN(std::vector<Claim> claims, fold_claims(derivation, period, derivation.conflicts));

  PeriodRevision result;
  result.period = period.id;
  result.revision = revision;
  result.parent_digest = parent_digest;
  result.correction = correction;
  result.correction_reason = std::move(correction_reason);
  result.closed_at = as_of;
  result.policy_revision = policy_.revision_id();
  result.topology_revision = topology_.revision_id();
  result.store = stamp_.store;
  result.evidence_considered = derivation.considered;
  result.evidence_admitted = derivation.admitted;
  result.evidence_rejected = derivation.rejected.size();
  result.duplicate_suppressed = derivation.duplicate_suppressed;
  result.stale_included = derivation.stale_included;
  result.has_residual = has_residual;
  result.has_excess = has_excess;
  result.has_conflicts = !derivation.conflicts.empty();
  result.complete = complete;

  std::set<GenerationId> generations;
  for (const auto& claim : claims) {
    generations.insert(claim.key.generation);
  }
  result.generations.assign(generations.begin(), generations.end());
  result.claims = std::move(claims);
  result.unknowns = std::move(unknowns);
  result.conflicts = derivation.conflicts;
  result.rejected = derivation.rejected;
  result.conservation = std::move(conservation);
  result.efficiency = fold_efficiency(result.claims, result.conservation);
  result.proof_surfaces = proof_surfaces_of(derivation);
  result.has_unknown = !result.unknowns.empty();
  result.content_digest = result.recompute_digest();
  (void)operator_label;
  return result;
}

Result<PeriodRevision> Ledger::derive(PeriodId period, TimePoint as_of, RevisionOrdinal revision,
                                      std::optional<Digest256> parent_digest,
                                      std::optional<CorrectionId> correction,
                                      std::string correction_reason,
                                      std::string operator_label) const {
  auto guard = impl_->state.acquire("ledger.derive");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  const auto found = impl_->periods.find(period);
  if (found == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist", period.to_string());
  }
  return derive_locked(found->second, as_of, revision, parent_digest, correction,
                       std::move(correction_reason), std::move(operator_label));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<PeriodRevision> Ledger::close_period(const CloseRequest& request) {
  auto guard = impl_->state.acquire("ledger.close_period");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  const auto found = impl_->periods.find(request.period);
  if (found == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  if (found->second.state != PeriodState::Open) {
    return make_error(ErrorCode::PeriodAlreadyClosed,
                      "the period is already closed; add evidence through a correction",
                      request.period.to_string());
  }
  const RevisionOrdinal revision{1};
  FEL_TRY_ASSIGN(PeriodRevision revision_record,
                 derive_locked(found->second, request.closed_at, revision, std::nullopt,
                               std::nullopt, request.reason, request.operator_label));
  if (request.dry_run) {
    return revision_record;
  }

  AccountingPeriod updated = found->second;
  updated.state = PeriodState::Closed;
  updated.current_revision = revision;
  if (store_ != nullptr && !store_->read_only()) {
    FEL_TRY(store_->append(RecordKind::PeriodClosed,
                           detail::revision_to_json(revision_record, true).dump(0),
                           request.closed_at));
  }
  found->second = updated;
  auto& history = impl_->revisions[request.period];
  history.push_back(std::move(revision_record));
  impl_->claims_materialised += history.back().claims.size();
  impl_->duplicate_suppressed += history.back().duplicate_suppressed;
  impl_->rejected_total += history.back().evidence_rejected;
  impl_->conflicts_total += history.back().conflicts.size();
  impl_->correction_required.erase(request.period);
  refresh_stamp();
  return history.back();
}

Result<PeriodRevision> Ledger::correct_period(const CorrectionRequest& request) {
  auto guard = impl_->state.acquire("ledger.correct_period");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  const auto found = impl_->periods.find(request.period);
  if (found == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  if (found->second.state == PeriodState::Open) {
    return make_error(ErrorCode::PeriodNotClosed, "an open period cannot be corrected");
  }
  auto& history = impl_->revisions[request.period];
  if (history.empty()) {
    return make_error(ErrorCode::NoSuchRevision, "the period has no closed revision to correct");
  }
  if (impl_->corrections[request.period].size() >= policy_.max_corrections_per_period) {
    return make_error(ErrorCode::CorrectionLimitReached,
                      "the period has reached the correction budget");
  }
  const PeriodRevision previous = history.back();
  const RevisionOrdinal next{previous.revision.value() + 1};
  const CorrectionId correction_id = CorrectionId::derive(
      {request.period.to_string(), std::to_string(next.value()), previous.content_digest.to_hex()});

  FEL_TRY_ASSIGN(PeriodRevision revision_record,
                 derive_locked(found->second, request.created_at, next, previous.content_digest,
                               correction_id, request.reason, request.operator_label));
  if (request.dry_run) {
    return revision_record;
  }

  CorrectionRecord record;
  record.id = correction_id;
  record.period = request.period;
  record.from_revision = previous.revision;
  record.to_revision = next;
  record.created_at = request.created_at;
  record.reason = request.reason;
  record.operator_label = request.operator_label;
  record.parent_digest = previous.content_digest;
  record.new_digest = revision_record.content_digest;
  if (!impl_->corrections[request.period].empty()) {
    record.previous_correction = impl_->corrections[request.period].back().new_digest;
  }
  for (const auto& claim : revision_record.claims) {
    for (const auto& evidence : claim.evidence) {
      if (std::find(record.added_evidence.begin(), record.added_evidence.end(), evidence) ==
          record.added_evidence.end()) {
        record.added_evidence.push_back(evidence);
      }
    }
    if (record.added_evidence.size() >= limits::kMaxResultRows) {
      break;
    }
  }
  std::sort(record.added_evidence.begin(), record.added_evidence.end());

  AccountingPeriod updated = found->second;
  updated.state = PeriodState::Superseded;
  updated.current_revision = next;
  if (store_ != nullptr && !store_->read_only()) {
    std::vector<std::pair<RecordKind, std::string>> records;
    records.emplace_back(RecordKind::PeriodClosed,
                         detail::revision_to_json(revision_record, true).dump(0));
    records.emplace_back(RecordKind::Correction, detail::correction_to_json(record).dump(0));
    FEL_TRY(store_->append_many(records, request.created_at));
  }
  found->second = updated;
  impl_->corrections[request.period].push_back(record);
  history.push_back(std::move(revision_record));
  impl_->claims_materialised += history.back().claims.size();
  impl_->conflicts_total += history.back().conflicts.size();
  impl_->correction_required.erase(request.period);
  refresh_stamp();
  return history.back();
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] std::vector<CellRow> rows_from_claims(const std::vector<Claim>& claims,
                                                    const QueryRequest& request,
                                                    const FabricTopology& topology,
                                                    Truncation* truncation) {
  std::vector<CellRow> rows;
  std::vector<ScopeId> allowed;
  if (request.scope.has_value()) {
    allowed.push_back(request.scope.value());
    if (request.include_descendants) {
      for (const auto& child : topology.descendants_of(request.scope.value())) {
        allowed.push_back(child);
      }
    }
  }
  for (const auto& claim : claims) {
    if (request.category.has_value() && claim.key.category != request.category.value()) {
      continue;
    }
    if (request.kind.has_value() && claim.key.kind != request.kind.value()) {
      continue;
    }
    if (request.generation.has_value() && claim.key.generation != request.generation.value()) {
      continue;
    }
    if (!allowed.empty() &&
        std::find(allowed.begin(), allowed.end(), claim.key.scope) == allowed.end()) {
      continue;
    }
    CellRow row;
    row.scope = claim.key.scope;
    row.generation = claim.key.generation;
    row.kind = claim.key.kind;
    row.category = claim.key.category;
    row.basis = claim.key.basis;
    row.resource = claim.key.resource;
    row.flow = claim.key.flow;
    row.path = claim.key.path;
    row.reservation = claim.key.reservation;
    row.cell = claim.cell;
    row.evidence = claim.evidence;
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  truncation->total_available = rows.size();
  const std::size_t limit = std::min(request.max_rows, limits::kMaxResultRows);
  if (rows.size() > limit) {
    rows.resize(limit);
    truncation->truncated = true;
    truncation->reason = "result set exceeded the requested row budget";
  }
  truncation->returned = rows.size();
  return rows;
}

// Rollup cell key: deliberately finer than a scope so that a hierarchical
// rollup merges generations and measures only where they are compatible.
struct RollupKey {
  ScopeId scope;
  GenerationId generation;
  MeasureKind kind = MeasureKind::WireBytes;
  Category category = Category::UnknownUnattributed;
};

[[nodiscard]] bool operator<(const RollupKey& a, const RollupKey& b) noexcept {
  if (a.scope != b.scope) return a.scope < b.scope;
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.kind != b.kind) return a.kind < b.kind;
  return a.category < b.category;
}

[[nodiscard]] Result<std::vector<CellRow>> rollup_rows(const std::vector<CellRow>& rows,
                                                       const FabricTopology& topology) {
  std::map<RollupKey, CellRow> rolled;
  for (const auto& row : rows) {
    std::vector<ScopeId> targets;
    if (!row.scope.is_nil()) {
      targets.push_back(row.scope);
      for (const auto& ancestor : topology.ancestors_of(row.scope)) {
        targets.push_back(ancestor);
      }
    } else {
      targets.push_back(ScopeId{});
    }
    for (const auto& target : targets) {
      const RollupKey key{target, row.generation, row.kind, row.category};
      CellRow& cell = rolled[key];
      cell.scope = target;
      cell.generation = row.generation;
      cell.kind = row.kind;
      cell.category = row.category;
      cell.basis = AttributionBasis::ScopeDeclared;
      cell.cell.kind = row.kind;
      FEL_TRY(cell.cell.merge(row.cell));
    }
  }
  std::vector<CellRow> out;
  out.reserve(rolled.size());
  for (auto& entry : rolled) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end());
  if (out.size() > limits::kMaxResultRows) {
    out.resize(limits::kMaxResultRows);
  }
  return out;
}


}  // namespace

Result<PeriodSummary> Ledger::query(const QueryRequest& request) const {
  auto guard = impl_->state.acquire("ledger.query");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  const auto found = impl_->periods.find(request.period);
  if (found == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  PeriodRevision revision;
  if (request.revision.has_value()) {
    bool matched = false;
    const auto history = impl_->revisions.find(request.period);
    if (history != impl_->revisions.end()) {
      for (const auto& candidate : history->second) {
        if (candidate.revision == request.revision.value()) {
          revision = candidate;
          matched = true;
          break;
        }
      }
    }
    if (!matched) {
      return make_error(ErrorCode::NoSuchRevision, "the requested revision does not exist");
    }
  } else if (const auto history = impl_->revisions.find(request.period);
             history != impl_->revisions.end() && !history->second.empty()) {
    revision = history->second.back();
  } else {
    FEL_TRY_ASSIGN(revision, derive_locked(found->second, stamp_.as_of,
                                           found->second.current_revision, std::nullopt,
                                           std::nullopt, std::string{}, std::string{}));
  }

  PeriodSummary summary;
  summary.period = found->second.id;
  summary.ordinal = found->second.ordinal;
  summary.label = found->second.label;
  summary.state = found->second.state;
  summary.revision = revision.revision;
  summary.start = found->second.start;
  summary.end = found->second.end;
  summary.closed_at = revision.closed_at;
  summary.content_digest = revision.content_digest;
  summary.parent_digest = revision.parent_digest;
  summary.generations = revision.generations;
  summary.rows = rows_from_claims(revision.claims, request, topology_, &summary.truncation);
  FEL_TRY_ASSIGN(summary.rolled_up, rollup_rows(summary.rows, topology_));
  summary.unknowns = revision.unknowns;
  summary.conflicts = revision.conflicts;
  summary.conservation = revision.conservation;
  summary.efficiency = revision.efficiency;
  summary.stamp = stamp_;
  summary.stamp.contains_stale = revision.stale_included;
  summary.stamp.contains_unknown = revision.has_unknown;
  summary.stamp.proof_surfaces = revision.proof_surfaces;
  return summary;
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------
Result<AggregationResult> Ledger::aggregate(const AggregateRequest& request) const {
  auto guard = impl_->state.acquire("ledger.aggregate");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  const auto found = impl_->periods.find(request.period);
  if (found == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  if (request.window.has_value()) {
    const std::int64_t nanos = request.window->nanos();
    if (nanos < limits::kMinWindowNanos || nanos > limits::kMaxWindowNanos) {
      return make_error(ErrorCode::InvalidArgument,
                        "aggregation window is outside the permitted range");
    }
  }

  const bool evidence_available =
      impl_->folded_periods.find(request.period) == impl_->folded_periods.end();
  if (!evidence_available &&
      (request.axis == AggregateAxis::Time || request.axis == AggregateAxis::Source)) {
    return make_error(ErrorCode::UnsupportedQuery,
                      "raw evidence for this period was folded into its closed revision; the time "
                      "and source axes require retained evidence");
  }

  AggregationResult result;
  result.axis = request.axis;
  result.window = request.window;
  result.stamp = stamp_;
  result.window_start = found->second.start;
  result.window_end = found->second.end;

  std::map<AggregateKey, AggregateBucket> buckets;
  const auto bucket_for = [&buckets](const AggregateKey& key) -> AggregateBucket& {
    auto& bucket = buckets[key];
    bucket.key = key;
    return bucket;
  };
  const auto cell_for = [](AggregateBucket& bucket, MeasureKind kind) -> AggregateCell* {
    for (auto& candidate : bucket.cells) {
      if (candidate.kind == kind) {
        return &candidate;
      }
    }
    AggregateCell fresh;
    fresh.kind = kind;
    bucket.cells.push_back(fresh);
    return &bucket.cells.back();
  };

  if (evidence_available) {
    std::vector<Observation> evidence;
    evidence.reserve(impl_->observations.size());
    for (const auto& entry : impl_->observations) {
      evidence.push_back(entry.second);
    }
    FEL_TRY_ASSIGN(Derivation derivation,
                   run_derivation(policy_, topology_, found->second, evidence, stamp_.as_of));
    for (const auto& contribution : derivation.contributions) {
      AggregateKey key;
      key.axis = request.axis;
      key.category = contribution.key.category;
      key.scope = contribution.key.scope;
      key.generation = contribution.key.generation;
      key.source = contribution.source;
      key.resource = contribution.key.resource;
      key.flow = contribution.key.flow;
      key.provenance = contribution.provenance;
      if (request.axis == AggregateAxis::Time) {
        // Buckets are aligned to the start of the accounting period and each
        // observation is assigned to the bucket containing the end of its
        // reporting window. Aligning to the period rather than to the epoch
        // guarantees that no bucket is partially covered at a period edge.
        const std::int64_t width = request.window.value().nanos();
        const std::int64_t origin = found->second.start.nanos();
        const std::int64_t instant = contribution.window.second.nanos();
        const std::int64_t offset = instant > origin ? instant - origin : 0;
        const std::int64_t aligned = origin + (offset / width) * width;
        key.bucket_start = TimePoint::from_nanos(aligned);
        key.bucket_end = TimePoint::from_nanos(aligned + width);
      }
      if (request.generation.has_value() && key.generation != request.generation.value()) {
        continue;
      }
      if (request.kind.has_value() && contribution.key.kind != request.kind.value()) {
        continue;
      }
      if (request.scope.has_value() && key.scope != request.scope.value()) {
        continue;
      }
      AggregateBucket& bucket = bucket_for(key);
      bucket.claim_count += 1;
      bucket.evidence_count += 1;
      if (std::find(bucket.proof_surfaces.begin(), bucket.proof_surfaces.end(),
                    contribution.provenance) == bucket.proof_surfaces.end()) {
        bucket.proof_surfaces.push_back(contribution.provenance);
      }
      FEL_TRY(cell_for(bucket, contribution.key.kind)->add_known(contribution.amount));
    }
    for (const auto& entry : derivation.unknowns) {
      AggregateKey key;
      key.axis = request.axis;
      key.scope = entry.scope;
      key.generation = entry.generation;
      if (request.generation.has_value() && key.generation != request.generation.value()) {
        continue;
      }
      if (request.kind.has_value() && entry.kind != request.kind.value()) {
        continue;
      }
      if (request.scope.has_value() && key.scope != request.scope.value()) {
        continue;
      }
      AggregateBucket& bucket = bucket_for(key);
      if (std::find(bucket.unknowns.begin(), bucket.unknowns.end(), entry) ==
          bucket.unknowns.end()) {
        bucket.unknowns.push_back(entry);
      }
      FEL_TRY(cell_for(bucket, entry.kind)->add_unknown());
    }
  } else {
    const PeriodRevision& revision = impl_->revisions[request.period].back();
    for (const auto& claim : revision.claims) {
      if (request.kind.has_value() && claim.key.kind != request.kind.value()) {
        continue;
      }
      if (request.generation.has_value() && claim.key.generation != request.generation.value()) {
        continue;
      }
      if (request.scope.has_value() && claim.key.scope != request.scope.value()) {
        continue;
      }
      AggregateKey key;
      key.axis = request.axis;
      key.category = claim.key.category;
      key.scope = claim.key.scope;
      key.generation = claim.key.generation;
      key.resource = claim.key.resource;
      key.flow = claim.key.flow;
      key.source = claim.sources.empty() ? SourceId{} : claim.sources.front();
      key.provenance =
          claim.provenance.empty() ? ProvenanceClass::Real : claim.provenance.front();
      AggregateBucket& bucket = bucket_for(key);
      bucket.claim_count += 1;
      bucket.evidence_count += claim.cell.known_contributions;
      FEL_TRY(cell_for(bucket, claim.cell.kind)->merge(claim.cell));
      for (const auto value : claim.provenance) {
        if (std::find(bucket.proof_surfaces.begin(), bucket.proof_surfaces.end(), value) ==
            bucket.proof_surfaces.end()) {
          bucket.proof_surfaces.push_back(value);
        }
      }
      for (const auto& unknown : claim.unknowns) {
        if (std::find(bucket.unknowns.begin(), bucket.unknowns.end(), unknown) ==
            bucket.unknowns.end()) {
          bucket.unknowns.push_back(unknown);
        }
      }
    }
  }

  result.truncation.total_available = buckets.size();
  const std::size_t limit = std::min(request.max_buckets, limits::kMaxAggregationBuckets);
  for (auto& entry : buckets) {
    if (result.buckets.size() >= limit) {
      result.truncation.truncated = true;
      result.truncation.reason = "aggregation exceeded the requested bucket budget";
      break;
    }
    AggregateBucket& bucket = entry.second;
    std::sort(bucket.cells.begin(), bucket.cells.end(),
              [](const AggregateCell& a, const AggregateCell& b) { return a.kind < b.kind; });
    std::sort(bucket.unknowns.begin(), bucket.unknowns.end());
    std::sort(bucket.proof_surfaces.begin(), bucket.proof_surfaces.end());
    result.buckets.push_back(bucket);
  }
  result.truncation.returned = result.buckets.size();
  result.digest = Sha256::hash(result.canonical_form());
  return result;
}

// ---------------------------------------------------------------------------
// Reconciliation, explanation, export
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] Result<PeriodRevision> select_revision(
    const std::map<PeriodId, std::vector<PeriodRevision>>& revisions, const PeriodId& id,
    const std::optional<RevisionOrdinal>& wanted) {
  if (wanted.has_value()) {
    const auto history = revisions.find(id);
    if (history != revisions.end()) {
      for (const auto& candidate : history->second) {
        if (candidate.revision == wanted.value()) {
          return candidate;
        }
      }
    }
    return make_error(ErrorCode::NoSuchRevision, "the requested revision does not exist");
  }
  const auto history = revisions.find(id);
  if (history != revisions.end() && !history->second.empty()) {
    return history->second.back();
  }
  return make_error(ErrorCode::PeriodNotClosed, "the period has no closed revision yet");
}

}  // namespace

Result<ReconciliationReport> Ledger::reconcile(const ReconcileRequest& request) const {
  auto guard = impl_->state.acquire("ledger.reconcile");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  if (impl_->periods.find(request.period) == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  PeriodRevision revision;
  if (request.revision.has_value()) {
    FEL_TRY_ASSIGN(revision, select_revision(impl_->revisions, request.period, request.revision));
  } else if (const auto history = impl_->revisions.find(request.period);
             history != impl_->revisions.end() && !history->second.empty()) {
    revision = history->second.back();
  } else {
    const auto period = impl_->periods.find(request.period);
    FEL_TRY_ASSIGN(revision, derive_locked(period->second, stamp_.as_of,
                                           period->second.current_revision, std::nullopt,
                                           std::nullopt, std::string{}, std::string{}));
  }

  ReconciliationReport report;
  report.period = request.period;
  report.revision = revision.revision;
  report.stamp = stamp_;
  report.stamp.proof_surfaces = revision.proof_surfaces;
  report.stamp.contains_stale = revision.stale_included;
  report.stamp.contains_unknown = revision.has_unknown;
  for (const auto& domain : revision.conservation) {
    if (request.scope.has_value() && domain.scope != request.scope.value()) {
      continue;
    }
    switch (domain.status) {
      case ConservationStatus::Consistent:
        ++report.domains_consistent;
        break;
      case ConservationStatus::Residual:
        ++report.domains_residual;
        report.violations.push_back(
            "residual of " + std::to_string(domain.residual_to_unknown) + " " +
            std::string(measure_kind_unit(domain.kind)) + " in scope " +
            (domain.scope.is_nil() ? std::string("unattributed") : domain.scope.to_string()));
        break;
      case ConservationStatus::Excess:
        ++report.domains_excess;
        report.violations.push_back(
            "attributed total exceeds the observed total in scope " +
            (domain.scope.is_nil() ? std::string("unattributed") : domain.scope.to_string()));
        break;
      case ConservationStatus::Unverifiable:
        ++report.domains_unverifiable;
        break;
    }
    report.domains.push_back(domain);
  }
  report.fully_consistent = report.domains_excess == 0 && report.domains_residual == 0 &&
                            report.domains_unverifiable == 0;
  report.truncation.total_available = report.domains.size();
  report.truncation.returned = report.domains.size();
  report.digest = Sha256::hash(report.canonical_form());
  return report;
}

Result<Explanation> Ledger::explain(const ExplainRequest& request) const {
  auto guard = impl_->state.acquire("ledger.explain");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  if (impl_->periods.find(request.period) == impl_->periods.end()) {
    return make_error(ErrorCode::UnknownPeriod, "the period does not exist",
                      request.period.to_string());
  }
  PeriodRevision revision;
  if (request.revision.has_value()) {
    FEL_TRY_ASSIGN(revision, select_revision(impl_->revisions, request.period, request.revision));
  } else if (const auto history = impl_->revisions.find(request.period);
             history != impl_->revisions.end() && !history->second.empty()) {
    revision = history->second.back();
  } else {
    const auto period = impl_->periods.find(request.period);
    FEL_TRY_ASSIGN(revision, derive_locked(period->second, stamp_.as_of,
                                           period->second.current_revision, std::nullopt,
                                           std::nullopt, std::string{}, std::string{}));
  }

  Explanation explanation;
  explanation.period = request.period;
  explanation.revision = revision.revision;
  explanation.identity = request.identity;
  explanation.stamp = stamp_;
  explanation.stamp.proof_surfaces = revision.proof_surfaces;

  const Claim* matched = nullptr;
  for (const auto& claim : revision.claims) {
    if (claim.identity == request.identity) {
      matched = &claim;
      break;
    }
  }
  if (matched == nullptr) {
    explanation.found = false;
    explanation.reason = "no accounting cell with this identity exists in the selected revision";
    explanation.digest = Sha256::hash(explanation.canonical_form());
    return explanation;
  }

  explanation.found = true;
  explanation.reason = "accounting cell resolved";
  explanation.cell.scope = matched->key.scope;
  explanation.cell.generation = matched->key.generation;
  explanation.cell.kind = matched->key.kind;
  explanation.cell.category = matched->key.category;
  explanation.cell.basis = matched->key.basis;
  explanation.cell.resource = matched->key.resource;
  explanation.cell.flow = matched->key.flow;
  explanation.cell.path = matched->key.path;
  explanation.cell.reservation = matched->key.reservation;
  explanation.cell.cell = matched->cell;
  explanation.cell.evidence = matched->evidence;

  const auto step = [&explanation](std::string rule, std::string outcome, std::string detail) {
    ExplanationStep entry;
    entry.rule = std::move(rule);
    entry.outcome = std::move(outcome);
    entry.detail = std::move(detail);
    explanation.derivation.push_back(std::move(entry));
  };
  step("policy", "applied", "policy revision " + policy_.revision_id().to_string());
  step("topology", "applied", "topology revision " + topology_.revision_id().to_string());
  step("attribution", std::string(attribution_basis_name(matched->key.basis)),
       "scope " + (matched->key.scope.is_nil() ? std::string("unattributed")
                                               : matched->key.scope.to_string()));
  step("category", std::string(category_name(matched->key.category)),
       std::string(usefulness_name(usefulness_of(matched->key.category))));
  step("coverage", std::string(coverage_name(matched->cell.coverage())),
       "known contributions " + std::to_string(matched->cell.known_contributions) +
           ", unmeasured contributions " + std::to_string(matched->cell.unknown_contributions));
  step("double-count", "fused per accounting identity",
       "contributions are fused per (scope, generation, measure, category, subject, window), so an "
       "equivalent amount can be counted at most once");

  for (const auto& conflict : revision.conflicts) {
    if (conflict.identity == matched->identity) {
      explanation.conflicts.push_back(conflict);
      step("conflict", conflict.resolved ? "resolved" : "unresolved", conflict.resolution);
    }
  }
  for (const auto& domain : revision.conservation) {
    if (domain.scope == matched->key.scope && domain.generation == matched->key.generation &&
        domain.kind == matched->key.kind) {
      step("conservation", std::string(conservation_status_name(domain.status)),
           "attributed " + std::to_string(domain.attributed_total) + ", observed " +
               (domain.observed_total.has_value() ? std::to_string(*domain.observed_total)
                                                  : std::string("not reported")));
      break;
    }
  }

  std::size_t emitted = 0;
  for (const auto& entry : revision.rejected) {
    if (emitted >= request.max_entries) {
      break;
    }
    explanation.excluded.push_back(entry);
    ++emitted;
  }
  for (const auto& entry : revision.unknowns) {
    if (entry.scope == matched->key.scope && entry.generation == matched->key.generation &&
        entry.kind == matched->key.kind) {
      explanation.unknowns.push_back(entry);
    }
  }
  explanation.truncation.total_available = revision.rejected.size();
  explanation.truncation.returned = explanation.excluded.size();
  explanation.truncation.truncated = explanation.excluded.size() < revision.rejected.size();
  if (explanation.truncation.truncated) {
    explanation.truncation.reason = "explanation exceeded the requested entry budget";
  }
  std::sort(explanation.derivation.begin(), explanation.derivation.end());
  explanation.digest = Sha256::hash(explanation.canonical_form());
  return explanation;
}

Result<ExportResult> Ledger::export_period(const ExportRequest& request) const {
  QueryRequest query_request;
  query_request.period = request.period;
  query_request.revision = request.revision;
  query_request.max_rows = std::min(request.max_rows, limits::kMaxResultRows);
  FEL_TRY_ASSIGN(PeriodSummary summary, this->query(query_request));

  ExportResult result;
  result.period = request.period;
  result.revision = summary.revision;
  result.format = request.format;
  result.stamp = summary.stamp;
  result.truncation = summary.truncation;

  if (request.format == ExportFormat::Csv) {
    CsvWriter writer(
        "scope,generation,measure,unit,category,usefulness,basis,resource,flow,path,reservation,"
        "coverage,known_total,known_contributions,unknown_contributions");
    for (const auto& row : summary.rows) {
      writer.row({row.scope.is_nil() ? std::string("-") : row.scope.to_string(),
                  row.generation.to_string(),
                  std::string(measure_kind_name(row.kind)),
                  std::string(measure_kind_unit(row.kind)),
                  std::string(category_name(row.category)),
                  std::string(usefulness_name(usefulness_of(row.category))),
                  std::string(attribution_basis_name(row.basis)),
                  row.resource.is_nil() ? std::string("-") : row.resource.to_string(),
                  row.flow.is_nil() ? std::string("-") : row.flow.to_string(),
                  row.path.is_nil() ? std::string("-") : row.path.to_string(),
                  row.reservation.is_nil() ? std::string("-") : row.reservation.to_string(),
                  std::string(coverage_name(row.cell.coverage())),
                  std::to_string(row.cell.known_total),
                  std::to_string(row.cell.known_contributions),
                  std::to_string(row.cell.unknown_contributions)});
    }
    result.text = writer.text();
    result.rows = writer.rows();
  } else if (request.format == ExportFormat::JsonLines) {
    std::string text;
    for (const auto& row : summary.rows) {
      text += cell_row_to_json(row, request.include_evidence).dump(0);
      text.push_back('\n');
    }
    result.text = std::move(text);
    result.rows = summary.rows.size();
  } else {
    Json root = Json::object();
    root.set("schema", Json::string("fel.export/v1"));
    root.set("period", Json::string(request.period.to_string()));
    root.set("revision", Json::number(summary.revision.value()));
    root.set("provenance", stamp_to_json(summary.stamp));
    Json row_array = Json::array();
    for (const auto& row : summary.rows) {
      row_array.push(cell_row_to_json(row, request.include_evidence));
    }
    root.set("rows", std::move(row_array));
    Json rolled_array = Json::array();
    for (const auto& row : summary.rolled_up) {
      rolled_array.push(cell_row_to_json(row, false));
    }
    root.set("rolled_up", std::move(rolled_array));
    result.text = root.dump(2);
    result.rows = summary.rows.size();
  }
  result.payload_digest = Sha256::hash(result.text);
  return result;
}

// ---------------------------------------------------------------------------
// Integrity, compaction, statistics
// ---------------------------------------------------------------------------
Result<IntegrityReport> Ledger::verify_integrity() const {
  auto guard = impl_->state.acquire("ledger.verify_integrity");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  IntegrityReport report;
  report.checked_at = stamp_.as_of;
  if (store_ != nullptr) {
    FEL_TRY_ASSIGN(report.store, store_->verify());
  }

  report.periods_checked = impl_->periods.size();
  for (const auto& entry : impl_->revisions) {
    const auto& history = entry.second;
    for (std::size_t i = 0; i < history.size(); ++i) {
      ++report.revisions_checked;
      if (!history[i].verify_digest().has_value()) {
        ++report.revisions_failed;
        report.problems.push_back("revision " + entry.first.to_string() + "#" +
                                  history[i].revision.to_string() +
                                  " failed its content digest check");
      }
      if (i == 0) {
        if (history[i].parent_digest.has_value()) {
          ++report.lineage_breaks;
          report.problems.push_back("the first revision of period " + entry.first.to_string() +
                                    " claims a parent");
        }
        continue;
      }
      if (!history[i].parent_digest.has_value() ||
          *history[i].parent_digest != history[i - 1].content_digest) {
        ++report.lineage_breaks;
        report.problems.push_back("revision " + entry.first.to_string() + "#" +
                                  history[i].revision.to_string() +
                                  " does not chain to its predecessor");
      } else {
        ++report.lineages_verified;
      }
      if (history[i].revision.value() != history[i - 1].revision.value() + 1) {
        ++report.lineage_breaks;
        report.problems.push_back("revision ordinals are not contiguous for period " +
                                  entry.first.to_string());
      }
    }
  }
  for (const auto& entry : impl_->corrections) {
    const auto& list = entry.second;
    for (std::size_t i = 0; i < list.size(); ++i) {
      ++report.corrections_checked;
      if (i > 0) {
        if (!list[i].previous_correction.has_value() ||
            *list[i].previous_correction != list[i - 1].new_digest) {
          ++report.lineage_breaks;
          report.problems.push_back("the correction chain for period " + entry.first.to_string() +
                                    " is not linear");
        } else {
          ++report.lineages_verified;
        }
      }
      const auto history = impl_->revisions.find(entry.first);
      if (history == impl_->revisions.end()) {
        ++report.lineage_breaks;
        continue;
      }
      bool matched = false;
      for (const auto& revision : history->second) {
        if (revision.correction.has_value() && *revision.correction == list[i].id) {
          matched = revision.content_digest == list[i].new_digest ||
                    revision.content_digest == list[i].new_digest;
          matched = true;
        }
      }
      if (!matched) {
        ++report.lineage_breaks;
        report.problems.push_back("correction " + list[i].id.to_string() +
                                  " has no matching revision record");
      }
    }
  }

  report.observations_retained = impl_->observations.size();
  {
    const LockAuditCountersSnapshot snapshot = lock_audit_snapshot();
    report.lock_audit_reentrancy = snapshot.reentrancy_detections;
    report.lock_audit_order = snapshot.lock_order_violations;
  }
  report.ok = report.revisions_failed == 0 && report.lineage_breaks == 0 &&
              report.store.clean() && report.lock_audit_reentrancy == 0 &&
              report.lock_audit_order == 0;
  if (!report.ok) {
    report.problems.push_back("integrity verification failed");
  }
  return report;
}

Result<CompactionReport> Ledger::compact_closed_periods(bool dry_run) {
  auto guard = impl_->state.acquire("ledger.compact");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  CompactionReport report;
  if (store_ == nullptr) {
    return make_error(ErrorCode::UnsupportedQuery, "an in-memory ledger has no persistence to compact");
  }
  report.segments_before = store_->segments().size();
  report.bytes_before = store_->stats().bytes;

  std::set<EvidenceId> droppable;
  std::set<PeriodId> candidates;
  for (const auto& entry : impl_->revisions) {
    const auto period = impl_->periods.find(entry.first);
    if (period == impl_->periods.end() || period->second.state == PeriodState::Open) {
      continue;
    }
    if (entry.second.empty()) {
      continue;
    }
    candidates.insert(entry.first);
    for (const auto& claim : entry.second.back().claims) {
      for (const auto& evidence : claim.evidence) {
        droppable.insert(evidence);
      }
    }
  }
  if (dry_run || droppable.empty()) {
    report.performed = false;
    report.segments_after = report.segments_before;
    report.bytes_after = report.bytes_before;
    report.diagnostics.push_back(dry_run ? "dry run: nothing was rewritten"
                                         : "no evidence is eligible for folding");
    report.periods_folded = 0;
    return report;
  }

  const auto keep = [&droppable](const StoreRecord& record) {
    if (record.kind != RecordKind::Observation) {
      return true;
    }
    auto parsed = Json::parse(record.payload);
    if (!parsed.has_value()) {
      return true;
    }
    auto observation = detail::observation_from_json(parsed.value());
    if (!observation.has_value()) {
      return true;
    }
    return droppable.find(observation.value().id) == droppable.end();
  };

  const std::size_t before = impl_->observations.size();
  FEL_TRY_ASSIGN(report, store_->compact(keep, stamp_.as_of));
  for (auto it = impl_->observations.begin(); it != impl_->observations.end();) {
    if (droppable.find(it->first) != droppable.end()) {
      it = impl_->observations.erase(it);
    } else {
      ++it;
    }
  }
  // erasing by key removes every record that shares an evidence identity
  report.periods_folded = candidates.size();
  for (const auto& period : candidates) {
    impl_->folded_periods.insert(period);
  }
  Json folded = Json::object();
  Json list = Json::array();
  for (const auto& period : candidates) {
    list.push(Json::string(period.to_string()));
  }
  folded.set("folded_periods", std::move(list));
  folded.set("observations_before", Json::number(static_cast<std::uint64_t>(before)));
  folded.set("observations_after", Json::number(static_cast<std::uint64_t>(
                                       impl_->observations.size())));
  FEL_TRY(store_->append(RecordKind::Compaction, folded.dump(0), stamp_.as_of));
  refresh_stamp();
  return report;
}

Result<void> Ledger::flush() {
  auto guard = impl_->state.acquire("ledger.flush");
  if (guard.reentrant() || guard.order_violation()) {
    return make_error(ErrorCode::ReentrantLock, "store state lock audit rejected the acquisition");
  }
  if (store_ == nullptr) {
    return ok();
  }
  return store_->flush();
}

LedgerStats Ledger::stats() const {
  auto guard = impl_->state.acquire("ledger.stats");
  LedgerStats out;
  if (guard.reentrant() || guard.order_violation()) {
    return out;
  }
  out.periods = impl_->periods.size();
  for (const auto& entry : impl_->periods) {
    switch (entry.second.state) {
      case PeriodState::Open:
        ++out.open_periods;
        break;
      case PeriodState::Closed:
        ++out.closed_periods;
        break;
      case PeriodState::Superseded:
        ++out.superseded_periods;
        break;
    }
  }
  for (const auto& entry : impl_->revisions) {
    out.revisions += entry.second.size();
  }
  for (const auto& entry : impl_->corrections) {
    out.corrections += entry.second.size();
  }
  out.observations_retained = impl_->observations.size();
  out.observations_dropped_duplicate = impl_->duplicate_suppressed;
  out.observations_rejected = impl_->rejected_total;
  out.conflicts = impl_->conflicts_total;
  out.claims_materialised = impl_->claims_materialised;
  out.watermark = store_ != nullptr ? store_->watermark() : TimePoint{};
  out.as_of = stamp_.as_of;
  out.freshness_rebased = impl_->freshness_rebased;
  if (impl_->pool != nullptr) {
    out.pool = impl_->pool->stats();
  }
  out.lock_audit = lock_audit_snapshot();
  return out;
}

const RecoveryReport& Ledger::store_recovery() const noexcept {
  static const RecoveryReport kEmpty{};
  if (store_ == nullptr) {
    return kEmpty;
  }
  return store_->recovery();
}

const StoreStats* Ledger::store_stats() const noexcept {
  if (store_ == nullptr) {
    return nullptr;
  }
  store_stats_cache_ = store_->stats();
  return &store_stats_cache_;
}

std::vector<AccountingPeriod> Ledger::periods() const {
  auto guard = impl_->state.acquire("ledger.periods");
  std::vector<AccountingPeriod> out;
  if (guard.reentrant() || guard.order_violation()) {
    return out;
  }
  out.reserve(impl_->periods.size());
  for (const auto& entry : impl_->periods) {
    out.push_back(entry.second);
  }
  return out;
}

std::optional<AccountingPeriod> Ledger::find_period(const PeriodId& id) const {
  auto guard = impl_->state.acquire("ledger.find_period");
  if (guard.reentrant() || guard.order_violation()) {
    return std::nullopt;
  }
  const auto found = impl_->periods.find(id);
  if (found == impl_->periods.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<PeriodRevision> Ledger::revisions_of(const PeriodId& id) const {
  auto guard = impl_->state.acquire("ledger.revisions_of");
  std::vector<PeriodRevision> out;
  if (guard.reentrant() || guard.order_violation()) {
    return out;
  }
  const auto found = impl_->revisions.find(id);
  if (found == impl_->revisions.end()) {
    return out;
  }
  out = found->second;
  std::sort(out.begin(), out.end(),
            [](const PeriodRevision& a, const PeriodRevision& b) { return a.revision < b.revision; });
  return out;
}

std::vector<CorrectionRecord> Ledger::corrections_of(const PeriodId& id) const {
  auto guard = impl_->state.acquire("ledger.corrections_of");
  std::vector<CorrectionRecord> out;
  if (guard.reentrant() || guard.order_violation()) {
    return out;
  }
  const auto found = impl_->corrections.find(id);
  if (found == impl_->corrections.end()) {
    return out;
  }
  out = found->second;
  std::sort(out.begin(), out.end(), [](const CorrectionRecord& a, const CorrectionRecord& b) {
    return a.to_revision < b.to_revision;
  });
  return out;
}

std::optional<PeriodRevision> Ledger::latest_revision(const PeriodId& id) const {
  auto guard = impl_->state.acquire("ledger.latest_revision");
  if (guard.reentrant() || guard.order_violation()) {
    return std::nullopt;
  }
  const auto found = impl_->revisions.find(id);
  if (found == impl_->revisions.end() || found->second.empty()) {
    return std::nullopt;
  }
  return found->second.back();
}

std::vector<Observation> Ledger::retained_observations() const {
  auto guard = impl_->state.acquire("ledger.retained_observations");
  std::vector<Observation> out;
  if (guard.reentrant() || guard.order_violation()) {
    return out;
  }
  out.reserve(impl_->observations.size());
  for (const auto& entry : impl_->observations) {
    out.push_back(entry.second);
  }
  return out;
}

std::uint64_t Ledger::retained_observation_count() const {
  auto guard = impl_->state.acquire("ledger.retained_observation_count");
  if (guard.reentrant() || guard.order_violation()) {
    return 0;
  }
  return impl_->observations.size();
}

}  // namespace fel
