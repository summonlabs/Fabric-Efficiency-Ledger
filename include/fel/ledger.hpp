// Fabric Efficiency Ledger - the accounting runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <functional>

#include "fel/aggregate.hpp"
#include "fel/claim.hpp"
#include "fel/concurrency.hpp"
#include "fel/id.hpp"
#include "fel/observation.hpp"
#include "fel/period.hpp"
#include "fel/persistence.hpp"
#include "fel/policy.hpp"
#include "fel/provenance.hpp"
#include "fel/topology.hpp"

namespace fel {

struct OpenOptions {
  std::string store_path;
  StoreOpenMode mode = StoreOpenMode::OpenOrCreate;
  LedgerPolicy policy{};
  FabricTopology topology{};
  // Explicit "as of" instant. When unset the store watermark is used, so a
  // restarted process can never silently promote persisted evidence to fresh.
  std::optional<TimePoint> as_of;
  bool rebase_freshness_on_open = true;
  bool take_store_lock = true;
  bool enable_worker_pool = false;
  std::uint32_t lock_timeout_ms = 0;
  std::uint32_t lock_hold_max_ms = 30000;
};

struct CloseRequest {
  PeriodId period;
  TimePoint closed_at;
  std::string reason;
  std::string operator_label;
  // When true the close is computed but nothing is persisted or promoted.
  bool dry_run = false;
};

struct CorrectionRequest {
  PeriodId period;
  TimePoint created_at;
  std::string reason;
  std::string operator_label;
  bool dry_run = false;
};

struct QueryRequest {
  PeriodId period;
  std::optional<RevisionOrdinal> revision;
  std::optional<ScopeId> scope;
  bool include_descendants = false;
  std::optional<Category> category;
  std::optional<MeasureKind> kind;
  std::optional<GenerationId> generation;
  bool include_unknowns = true;
  std::size_t max_rows = limits::kMaxResultRows;
};

// A rolled up, deterministic view of one period revision.
struct CellRow {
  ScopeId scope;
  GenerationId generation;
  MeasureKind kind = MeasureKind::WireBytes;
  Category category = Category::UnknownUnattributed;
  AttributionBasis basis = AttributionBasis::Unattributed;
  ResourceId resource;
  FlowId flow;
  PathId path;
  ReservationId reservation;
  AggregateCell cell;
  std::vector<EvidenceId> evidence;

  [[nodiscard]] std::string canonical_form() const;
  friend bool operator<(const CellRow& a, const CellRow& b) noexcept;
};

struct PeriodSummary {
  PeriodId period;
  Ordinal ordinal;
  std::string label;
  PeriodState state = PeriodState::Open;
  RevisionOrdinal revision;
  TimePoint start;
  TimePoint end;
  TimePoint closed_at;
  Digest256 content_digest;
  std::optional<Digest256> parent_digest;
  std::vector<GenerationId> generations;
  std::vector<CellRow> rows;
  std::vector<CellRow> rolled_up;  // hierarchical rollup along the scope tree
  std::vector<UnknownEntry> unknowns;
  std::vector<ConflictRecord> conflicts;
  std::vector<DomainConservation> conservation;
  std::vector<EfficiencySummary> efficiency;
  Truncation truncation;
  ProvenanceStamp stamp;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

struct ReconcileRequest {
  PeriodId period;
  std::optional<RevisionOrdinal> revision;
  std::optional<ScopeId> scope;
};

struct ReconciliationReport {
  PeriodId period;
  RevisionOrdinal revision;
  std::vector<DomainConservation> domains;
  std::uint64_t domains_consistent = 0;
  std::uint64_t domains_residual = 0;
  std::uint64_t domains_excess = 0;
  std::uint64_t domains_unverifiable = 0;
  std::uint64_t mixed_domain_rejections = 0;
  bool fully_consistent = false;
  std::vector<std::string> violations;
  Truncation truncation;
  ProvenanceStamp stamp;
  Digest256 digest;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

struct ExplainRequest {
  PeriodId period;
  AccountingIdentity identity;
  std::optional<RevisionOrdinal> revision;
  std::size_t max_entries = limits::kMaxExplanationEntries;
};

struct ExplanationStep {
  std::string rule;
  std::string outcome;
  std::string detail;

  friend bool operator<(const ExplanationStep& a, const ExplanationStep& b) noexcept {
    if (a.rule != b.rule) return a.rule < b.rule;
    if (a.outcome != b.outcome) return a.outcome < b.outcome;
    return a.detail < b.detail;
  }
};

struct Explanation {
  PeriodId period;
  RevisionOrdinal revision;
  bool found = false;
  std::string reason;
  AccountingIdentity identity;
  CellRow cell;
  std::vector<ExplanationStep> derivation;
  std::vector<UnknownEntry> unknowns;
  std::vector<ConflictRecord> conflicts;
  std::vector<RejectedObservation> excluded;
  std::vector<DomainConservation> conservation;
  Truncation truncation;
  ProvenanceStamp stamp;
  Digest256 digest;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

enum class ExportFormat : std::uint8_t {
  JsonLines = 1,
  Csv = 2,
  CanonicalJson = 3,
};

[[nodiscard]] std::string_view export_format_name(ExportFormat format) noexcept;
[[nodiscard]] std::optional<ExportFormat> export_format_from_name(std::string_view name) noexcept;

struct ExportRequest {
  PeriodId period;
  std::optional<RevisionOrdinal> revision;
  ExportFormat format = ExportFormat::JsonLines;
  bool include_evidence = true;
  std::size_t max_rows = limits::kMaxResultRows;
};

struct ExportResult {
  PeriodId period;
  RevisionOrdinal revision;
  ExportFormat format = ExportFormat::JsonLines;
  std::string text;
  std::uint64_t rows = 0;
  Digest256 payload_digest;
  Truncation truncation;
  ProvenanceStamp stamp;

  [[nodiscard]] std::string canonical_form() const;
};

struct AggregateRequest {
  PeriodId period;
  std::optional<RevisionOrdinal> revision;
  AggregateAxis axis = AggregateAxis::Category;
  std::optional<Duration> window;
  std::optional<ScopeId> scope;
  bool include_descendants = false;
  std::optional<GenerationId> generation;
  std::optional<MeasureKind> kind;
  std::size_t max_buckets = limits::kMaxAggregationBuckets;
};

struct IntegrityReport {
  bool ok = false;
  RecoveryReport store;
  std::uint64_t periods_checked = 0;
  std::uint64_t revisions_checked = 0;
  std::uint64_t revisions_failed = 0;
  std::uint64_t corrections_checked = 0;
  std::uint64_t lineages_verified = 0;
  std::uint64_t lineage_breaks = 0;
  std::uint64_t observations_retained = 0;
  std::uint64_t lock_audit_reentrancy = 0;
  std::uint64_t lock_audit_order = 0;
  std::vector<std::string> problems;
  TimePoint checked_at;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

struct LedgerStats {
  std::uint64_t periods = 0;
  std::uint64_t open_periods = 0;
  std::uint64_t closed_periods = 0;
  std::uint64_t superseded_periods = 0;
  std::uint64_t revisions = 0;
  std::uint64_t corrections = 0;
  std::uint64_t observations_retained = 0;
  std::uint64_t observations_dropped_duplicate = 0;
  std::uint64_t observations_rejected = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t claims_materialised = 0;
  TimePoint watermark;
  TimePoint as_of;
  bool freshness_rebased = false;
  PoolStats pool;
  LockAuditCountersSnapshot lock_audit;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

struct ClaimOptions {
  // Ingest to a specific period instead of inferring it from the window.
  std::optional<PeriodId> period;
  bool allow_late = true;
  bool stop_on_first_error = false;
};

// The accounting runtime. One instance owns one store, one policy revision and
// one topology revision. Every public operation is thread safe: mutations take
// the store state lock, reads take a consistent snapshot.
class Ledger {
 public:
  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;
  ~Ledger();

  [[nodiscard]] static Result<std::unique_ptr<Ledger>> open(const OpenOptions& options);
  // In-memory instance used for derivation only; nothing is persisted.
  [[nodiscard]] static Result<std::unique_ptr<Ledger>> in_memory(LedgerPolicy policy,
                                                                FabricTopology topology,
                                                                TimePoint as_of);

  [[nodiscard]] Result<PeriodId> open_period(TimePoint start, TimePoint end, std::string label);
  [[nodiscard]] Result<IngestReport> ingest(const ObservationBatch& batch,
                                            const ClaimOptions& options = {});
  [[nodiscard]] Result<IngestReport> ingest_parallel(const ObservationBatch& batch,
                                                     const ClaimOptions& options = {});

  [[nodiscard]] Result<PeriodRevision> close_period(const CloseRequest& request);
  [[nodiscard]] Result<PeriodRevision> correct_period(const CorrectionRequest& request);

  [[nodiscard]] Result<PeriodSummary> query(const QueryRequest& request) const;
  [[nodiscard]] Result<AggregationResult> aggregate(const AggregateRequest& request) const;
  [[nodiscard]] Result<ReconciliationReport> reconcile(const ReconcileRequest& request) const;
  [[nodiscard]] Result<Explanation> explain(const ExplainRequest& request) const;
  [[nodiscard]] Result<ExportResult> export_period(const ExportRequest& request) const;
  [[nodiscard]] Result<IntegrityReport> verify_integrity() const;
  [[nodiscard]] Result<CompactionReport> compact_closed_periods(bool dry_run);
  [[nodiscard]] Result<void> flush();

  // Derivation entry point shared by close/reconcile/explain. Pure function of
  // (policy, topology, retained evidence, period): no wall clock, no ordering
  // dependence, no hidden state.
  [[nodiscard]] Result<PeriodRevision> derive(PeriodId period, TimePoint as_of,
                                              RevisionOrdinal revision,
                                              std::optional<Digest256> parent_digest,
                                              std::optional<CorrectionId> correction,
                                              std::string correction_reason,
                                              std::string operator_label) const;

  [[nodiscard]] const LedgerPolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] const FabricTopology& topology() const noexcept { return topology_; }
  [[nodiscard]] const ProvenanceStamp& stamp() const noexcept { return stamp_; }
  [[nodiscard]] TimePoint as_of() const noexcept { return stamp_.as_of; }
  [[nodiscard]] LedgerStats stats() const;
  // Recovery outcome of the last store open. Empty for an in-memory ledger.
  [[nodiscard]] const RecoveryReport& store_recovery() const noexcept;
  [[nodiscard]] const StoreStats* store_stats() const noexcept;
  [[nodiscard]] std::vector<AccountingPeriod> periods() const;
  [[nodiscard]] std::optional<AccountingPeriod> find_period(const PeriodId& id) const;
  [[nodiscard]] std::vector<PeriodRevision> revisions_of(const PeriodId& id) const;
  [[nodiscard]] std::vector<CorrectionRecord> corrections_of(const PeriodId& id) const;
  [[nodiscard]] std::optional<PeriodRevision> latest_revision(const PeriodId& id) const;
  [[nodiscard]] std::vector<Observation> retained_observations() const;
  [[nodiscard]] std::uint64_t retained_observation_count() const;

 private:
  Ledger();

  struct Impl;

  // Derivation that assumes the store state lock is already held. Callers that
  // hold the lock must use this entry point; acquiring it twice would be a
  // reentrancy defect and is refused by the lock audit.
  [[nodiscard]] Result<PeriodRevision> derive_locked(const AccountingPeriod& period, TimePoint as_of,
                                                     RevisionOrdinal revision,
                                                     std::optional<Digest256> parent_digest,
                                                     std::optional<CorrectionId> correction,
                                                     std::string correction_reason,
                                                     std::string operator_label) const;
  void refresh_stamp() const;
  [[nodiscard]] TimePoint resolve_as_of(const std::optional<TimePoint>& requested) const;

  std::unique_ptr<Impl> impl_;

  LedgerPolicy policy_{};
  FabricTopology topology_{};
  mutable ProvenanceStamp stamp_{};
  mutable StoreStats store_stats_cache_{};
  std::unique_ptr<LedgerStore> store_;
};

// Deterministic period identity: derived from the interval, not from a counter.
[[nodiscard]] PeriodId derive_period_id(TimePoint start, TimePoint end);

}  // namespace fel
