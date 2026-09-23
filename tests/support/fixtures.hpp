// Fabric Efficiency Ledger - deterministic test fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fel/ledger.hpp"

namespace feltest {

// A small but structurally complete fabric: one root scope, one pod, one
// switch, two ports, two flows, one path and one reservation, with an explicit
// generation binding for every resource. Every identity is content derived, so
// the fixture is byte identical on every machine and every run.
struct Fixture {
  fel::FabricTopology topology;
  fel::GenerationId generation;
  fel::GenerationId superseded_generation;
  fel::SourceId counter_source;
  fel::SourceId probe_source;
  fel::SourceId unsupported_source;
  fel::IncarnationId counter_incarnation;
  fel::IncarnationId probe_incarnation;
  fel::ScopeId fabric_scope;
  fel::ScopeId pod_scope;
  fel::ScopeId switch_scope;
  fel::ScopeId port1_scope;
  fel::ScopeId port2_scope;
  fel::ResourceId port1;
  fel::ResourceId port2;
  fel::FlowId flow1;
  fel::PathId path1;
  fel::ReservationId reservation1;
  fel::TimePoint epoch0;
  fel::TimePoint period_start;
  fel::TimePoint period_end;
};

[[nodiscard]] Fixture make_fixture();

// Deterministic evidence builder. The caller states every dimension explicitly;
// nothing is inferred from a clock or from a random source.
struct ObservationSpec {
  fel::SourceId source;
  fel::IncarnationId incarnation;
  std::uint64_t epoch = 1;
  std::uint64_t sequence = 1;
  fel::GenerationId generation;
  std::uint64_t amount = 0;
  fel::Category category = fel::Category::UsefulDeliveredWork;
  fel::MeasureKind kind = fel::MeasureKind::WireBytes;
  fel::ObservationRole role = fel::ObservationRole::Contribution;
  std::optional<fel::ResourceId> resource;
  std::optional<fel::FlowId> flow;
  std::optional<fel::PathId> path;
  std::optional<fel::ReservationId> reservation;
  fel::TimePoint observed_at;
  fel::TimePoint received_at;
  fel::TimePoint window_start;
  fel::TimePoint window_end;
  std::optional<fel::OriginId> origin;
  std::string label;
};

[[nodiscard]] fel::Observation make_observation(const ObservationSpec& spec);

// A ledger policy tuned for deterministic tests: short freshness budgets so
// that stale and expired evidence can be exercised without waiting.
[[nodiscard]] fel::LedgerPolicy test_policy();

// A fixture plus an open, in-memory ledger with one open accounting period.
// Every integration test starts from this so that the accounting geometry is
// identical across the suite.
struct LedgerRig {
  Fixture fixture;
  std::unique_ptr<fel::Ledger> ledger;
  fel::PeriodId period;
};

[[nodiscard]] fel::Result<LedgerRig> make_memory_rig();
[[nodiscard]] fel::Result<LedgerRig> make_memory_rig_with(fel::LedgerPolicy policy);

// Convenience: a well formed contribution inside the fixture period.
[[nodiscard]] ObservationSpec make_spec(const Fixture& fixture, fel::SourceId source,
                                        fel::IncarnationId incarnation, std::uint64_t sequence,
                                        std::uint64_t amount, fel::Category category,
                                        std::optional<fel::ResourceId> resource);

[[nodiscard]] fel::ObservationBatch make_batch(const std::vector<fel::Observation>& observations);

// Scratch directory that removes itself. Used only by tests; never by the
// runtime.
class ScopedTempDir {
 public:
  explicit ScopedTempDir(const std::string& tag);
  ~ScopedTempDir();
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(const std::string& name) const;

 private:
  std::string path_;
};

}  // namespace feltest
