// Fabric Efficiency Ledger - smallest complete accounting round trip.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The example builds a two port fabric, records one accounting period of
// evidence, closes the period and prints the resulting totals. Every identity
// is content derived, so the printed values are identical on every run.
#include <cstdio>
#include <string>
#include <vector>

#include "example_support.hpp"

using namespace fel;

namespace {

// Convenience: a contribution inside a half open reporting window.
Observation contribution(const SourceId& source, const IncarnationId& incarnation,
                         const GenerationId& generation, const ResourceId& resource,
                         const TimePoint& window_start, const TimePoint& window_end,
                         std::uint64_t sequence, std::uint64_t amount, Category category) {
  Observation observation;
  observation.source = source;
  observation.incarnation = incarnation;
  observation.epoch = EpochId{1};
  observation.sequence = SourceSequence{sequence};
  observation.generation = generation;
  observation.observed_at = window_end;
  observation.received_at = window_end + Duration::from_seconds(1);
  observation.kind = MeasureKind::WireBytes;
  observation.role = ObservationRole::Contribution;
  observation.category = category;
  observation.resource = resource;
  observation.amount = amount;
  observation.window_start = window_start;
  observation.window_end = window_end;
  observation.origin = OriginId::derive({"example", std::to_string(sequence)});
  observation.id = EvidenceId::derive({"example", std::to_string(sequence)});
  return observation;
}

}  // namespace

int main() {
  const TimePoint period_start = TimePoint::from_seconds(1700000000);
  const TimePoint period_end = period_start + Duration::from_hours(1);

  const GenerationId generation = GenerationId::derive({"generation", "g1"});
  const SourceId source = SourceId::derive({"source", "switch-a"});
  const IncarnationId incarnation = IncarnationId::derive({"incarnation", "switch-a", "boot-1"});
  const ScopeId fabric = ScopeId::derive({"scope", "fabric"});
  const ScopeId switch_scope = ScopeId::derive({"scope", "switch-1"});
  const ScopeId port_scope = ScopeId::derive({"scope", "port-1"});
  const ResourceId port = ResourceId::derive({"resource", "port-1"});

  FabricTopology topology;
  Generation active;
  active.id = generation;
  active.ordinal = Ordinal{1};
  active.label = "generation-1";
  active.effective_from = period_start - Duration::from_days(1);
  static_cast<void>(topology.add_generation(active));

  Source agent;
  agent.id = source;
  agent.label = "switch-a counters";
  agent.kind = SourceKind::SwitchCounter;
  agent.authority = AuthorityRank{10};
  agent.provenance = ProvenanceClass::Real;
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
  resource.kind = ResourceKind::Port;
  resource.scope = port_scope;
  static_cast<void>(topology.add_resource(resource));

  GenerationBinding binding;
  binding.id = BindingId::derive({"binding", "port-1", "g1"});
  binding.resource = port;
  binding.generation = generation;
  binding.valid_from = period_start - Duration::from_days(1);
  static_cast<void>(topology.add_binding(binding));

  if (!topology.validate().has_value()) {
    std::printf("topology is invalid\n");
    return 1;
  }

  auto ledger = Ledger::in_memory(example::policy(), topology, period_end);
  if (!ledger.has_value()) {
    std::printf("ledger open failed: %s\n", ledger.error().to_string().c_str());
    return 1;
  }
  auto period = ledger.value()->open_period(period_start, period_end, "hour-1");
  if (!period.has_value()) {
    std::printf("period open failed: %s\n", period.error().to_string().c_str());
    return 1;
  }

  std::vector<Observation> evidence;
  evidence.push_back(contribution(source, incarnation, generation, port, period_start,
                                  period_start + Duration::from_minutes(10), 1, 900000,
                                  Category::UsefulDeliveredWork));
  evidence.push_back(contribution(source, incarnation, generation, port,
                                  period_start + Duration::from_minutes(10),
                                  period_start + Duration::from_minutes(20), 2, 40000,
                                  Category::Retransmission));
  evidence.push_back(contribution(source, incarnation, generation, port,
                                  period_start + Duration::from_minutes(20),
                                  period_start + Duration::from_minutes(30), 3, 10000,
                                  Category::ControlOverhead));

  ObservationBatch batch;
  batch.observations = evidence;
  batch.origin_label = "example";
  auto report = ledger.value()->ingest(batch);
  if (!report.has_value()) {
    std::printf("ingest failed: %s\n", report.error().to_string().c_str());
    return 1;
  }
  std::printf("ingest: received=%llu accepted=%llu\n",
              static_cast<unsigned long long>(report.value().received),
              static_cast<unsigned long long>(report.value().accepted));

  auto closed = ledger.value()->close_period(
      CloseRequest{period.value(), period_end, "example close", "example", false});
  if (!closed.has_value()) {
    std::printf("close failed: %s\n", closed.error().to_string().c_str());
    return 1;
  }
  std::printf("period digest: %s\n", closed.value().content_digest.to_hex().c_str());

  QueryRequest request;
  request.period = period.value();
  auto summary = ledger.value()->query(request);
  if (!summary.has_value()) {
    std::printf("query failed: %s\n", summary.error().to_string().c_str());
    return 1;
  }
  for (const auto& row : summary.value().rows) {
    std::printf("  %-24s %-12s total=%llu contributions=%llu\n",
                std::string(category_name(row.category)).c_str(),
                std::string(coverage_name(row.cell.coverage())).c_str(),
                static_cast<unsigned long long>(row.cell.known_total),
                static_cast<unsigned long long>(row.cell.known_contributions));
  }
  for (const auto& efficiency : summary.value().efficiency) {
    if (efficiency.denominator == 0 && efficiency.unknown_contributions == 0) {
      continue;
    }
    std::printf("  efficiency %-14s coverage=%s ratio=%s\n",
                std::string(measure_kind_name(efficiency.kind)).c_str(),
                std::string(coverage_name(efficiency.coverage)).c_str(),
                efficiency.ratio_text().c_str());
  }
  std::printf("proof surfaces: %s\n", summary.value().stamp.proof_surface_label().c_str());
  return 0;
}
