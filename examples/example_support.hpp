// Fabric Efficiency Ledger - shared scaffolding for the examples.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The basic and explanation examples spell every step out so that they can be
// read top to bottom. Examples that only need a populated ledger share this
// small helper instead of repeating the same setup.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fel/ledger.hpp"

namespace example {

using namespace fel;

// The library default freshness budget is five minutes, which suits near real
// time ingestion. A ledger that is closed once per hour needs a budget at least
// as wide as its reporting cadence, otherwise evidence produced early in the
// period is correctly classified as stale and excluded from the totals.
[[nodiscard]] inline LedgerPolicy policy() {
  LedgerPolicy ledger_policy;
  ledger_policy.freshness.fresh_ttl = Duration::from_hours(24);
  ledger_policy.freshness.stale_ttl = Duration::from_hours(48);
  return ledger_policy;
}

struct Environment {
  TimePoint period_start;
  TimePoint period_end;
  GenerationId generation;
  SourceId source;
  IncarnationId incarnation;
  ScopeId fabric_scope;
  ScopeId port_scope;
  ResourceId port;
  FabricTopology topology;
};

[[nodiscard]] inline Environment environment() {
  Environment env;
  env.period_start = TimePoint::from_seconds(1700000000);
  env.period_end = env.period_start + Duration::from_hours(1);
  env.generation = GenerationId::derive({"generation", "g1"});
  env.source = SourceId::derive({"source", "switch-a"});
  env.incarnation = IncarnationId::derive({"incarnation", "switch-a", "boot-1"});
  env.fabric_scope = ScopeId::derive({"scope", "fabric"});
  env.port_scope = ScopeId::derive({"scope", "port-1"});
  env.port = ResourceId::derive({"resource", "port-1"});

  Generation active;
  active.id = env.generation;
  active.ordinal = Ordinal{1};
  active.label = "g1";
  active.effective_from = env.period_start - Duration::from_days(1);
  (void)env.topology.add_generation(active);

  Source agent;
  agent.id = env.source;
  agent.label = "switch-a counters";
  agent.kind = SourceKind::SwitchCounter;
  agent.authority = AuthorityRank{10};
  agent.provenance = ProvenanceClass::Real;
  (void)env.topology.add_source(agent);

  SourceIncarnation boot;
  boot.id = env.incarnation;
  boot.source = env.source;
  boot.boot_epoch = EpochId{1};
  boot.started_at = env.period_start - Duration::from_days(1);
  (void)env.topology.add_incarnation(boot);

  Scope root;
  root.id = env.fabric_scope;
  root.label = "fabric";
  root.kind = ScopeKind::Fabric;
  (void)env.topology.add_scope(root);

  Scope leaf;
  leaf.id = env.port_scope;
  leaf.label = "port-1";
  leaf.kind = ScopeKind::Port;
  leaf.parent = env.fabric_scope;
  leaf.members = {env.port};
  (void)env.topology.add_scope(leaf);

  Resource resource;
  resource.id = env.port;
  resource.label = "port-1";
  resource.kind = ResourceKind::Port;
  resource.scope = env.port_scope;
  (void)env.topology.add_resource(resource);

  GenerationBinding binding;
  binding.id = BindingId::derive({"binding", "port-1", "g1"});
  binding.resource = env.port;
  binding.generation = env.generation;
  binding.valid_from = env.period_start - Duration::from_days(1);
  (void)env.topology.add_binding(binding);
  return env;
}

// Accumulates evidence into one batch. Each call uses a distinct reporting
// window so that the records are independent rather than contradictory.
class ObservationBatchBuilder {
 public:
  explicit ObservationBatchBuilder(const Environment& env) : env_(env) {}

  void add(std::uint64_t sequence, std::uint64_t amount, Category category,
           std::int64_t offset_minutes) {
    Observation observation;
    observation.source = env_.source;
    observation.incarnation = env_.incarnation;
    observation.epoch = EpochId{1};
    observation.sequence = SourceSequence{sequence};
    observation.generation = env_.generation;
    observation.window_start = env_.period_start + Duration::from_minutes(offset_minutes);
    observation.window_end = observation.window_start + Duration::from_minutes(10);
    observation.observed_at = observation.window_end;
    observation.received_at = observation.window_end + Duration::from_seconds(1);
    observation.kind = MeasureKind::WireBytes;
    observation.category = category;
    observation.resource = env_.port;
    observation.amount = amount;
    observation.origin = OriginId::derive({"example", std::to_string(sequence)});
    observation.id = EvidenceId::derive({"example", std::to_string(sequence)});
    observations_.push_back(observation);
  }

  [[nodiscard]] ObservationBatch batch() const {
    ObservationBatch batch;
    batch.observations = observations_;
    batch.origin_label = "example";
    return batch;
  }

  [[nodiscard]] std::size_t size() const noexcept { return observations_.size(); }

 private:
  Environment env_;
  std::vector<Observation> observations_;
};

}  // namespace example
