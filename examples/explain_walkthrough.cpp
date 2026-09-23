// Fabric Efficiency Ledger - reading an explanation trace.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shows how a single accounting cell can be traced back to the evidence, the
// policy rules and the conservation domain that produced it.
#include <cstdio>
#include <string>
#include <vector>

#include "example_support.hpp"

using namespace fel;

int main() {
  const TimePoint period_start = TimePoint::from_seconds(1700000000);
  const TimePoint period_end = period_start + Duration::from_hours(1);
  const GenerationId generation = GenerationId::derive({"generation", "g1"});
  const SourceId source = SourceId::derive({"source", "switch-a"});
  const IncarnationId incarnation = IncarnationId::derive({"incarnation", "switch-a", "boot-1"});
  const ScopeId fabric = ScopeId::derive({"scope", "fabric"});
  const ScopeId port_scope = ScopeId::derive({"scope", "port-1"});
  const ResourceId port = ResourceId::derive({"resource", "port-1"});

  FabricTopology topology;
  Generation active;
  active.id = generation;
  active.ordinal = Ordinal{1};
  active.label = "g1";
  active.effective_from = period_start - Duration::from_days(1);
  static_cast<void>(topology.add_generation(active));
  Source agent;
  agent.id = source;
  agent.label = "switch-a";
  agent.authority = AuthorityRank{10};
  static_cast<void>(topology.add_source(agent));
  SourceIncarnation boot;
  boot.id = incarnation;
  boot.source = source;
  boot.boot_epoch = EpochId{1};
  boot.started_at = period_start - Duration::from_days(1);
  static_cast<void>(topology.add_incarnation(boot));
  Scope root;
  root.id = fabric;
  root.label = "fabric";
  root.kind = ScopeKind::Fabric;
  static_cast<void>(topology.add_scope(root));
  Scope leaf;
  leaf.id = port_scope;
  leaf.label = "port-1";
  leaf.kind = ScopeKind::Port;
  leaf.parent = fabric;
  leaf.members = {port};
  static_cast<void>(topology.add_scope(leaf));
  Resource resource;
  resource.id = port;
  resource.label = "port-1";
  resource.scope = port_scope;
  static_cast<void>(topology.add_resource(resource));
  GenerationBinding binding;
  binding.id = BindingId::derive({"binding", "p1"});
  binding.resource = port;
  binding.generation = generation;
  binding.valid_from = period_start - Duration::from_days(1);
  static_cast<void>(topology.add_binding(binding));

  auto ledger = Ledger::in_memory(example::policy(), topology, period_end);
  if (!ledger.has_value()) {
    std::printf("ledger open failed\n");
    return 1;
  }
  auto period = ledger.value()->open_period(period_start, period_end, "hour-1");
  if (!period.has_value()) {
    std::printf("period open failed\n");
    return 1;
  }

  Observation observation;
  observation.source = source;
  observation.incarnation = incarnation;
  observation.epoch = EpochId{1};
  observation.sequence = SourceSequence{1};
  observation.generation = generation;
  observation.observed_at = period_start + Duration::from_minutes(10);
  observation.received_at = observation.observed_at;
  observation.kind = MeasureKind::WireBytes;
  observation.category = Category::Retransmission;
  observation.resource = port;
  observation.amount = 12345;
  observation.window_start = period_start;
  observation.window_end = period_start + Duration::from_minutes(10);
  observation.origin = OriginId::derive({"example", "1"});
  observation.id = EvidenceId::derive({"example", "1"});

  ObservationBatch batch;
  batch.observations = {observation};
  if (!ledger.value()->ingest(batch).has_value()) {
    std::printf("ingest failed\n");
    return 1;
  }
  auto closed = ledger.value()->close_period(
      CloseRequest{period.value(), period_end, "close", "example", false});
  if (!closed.has_value() || closed.value().claims.empty()) {
    std::printf("close failed or produced no cell\n");
    return 1;
  }

  ExplainRequest request;
  request.period = period.value();
  request.identity = closed.value().claims.front().identity;
  auto explanation = ledger.value()->explain(request);
  if (!explanation.has_value()) {
    std::printf("explain failed\n");
    return 1;
  }
  std::printf("cell %s\n", explanation.value().identity.to_string().c_str());
  std::printf("  %-16s %-24s %s\n", "rule", "outcome", "detail");
  for (const auto& step : explanation.value().derivation) {
    std::printf("  %-16s %-24s %s\n", step.rule.c_str(), step.outcome.c_str(), step.detail.c_str());
  }
  for (const auto& excluded : explanation.value().excluded) {
    std::printf("  excluded %s: %s\n", excluded.evidence.to_string().c_str(),
                excluded.detail.c_str());
  }
  return 0;
}
