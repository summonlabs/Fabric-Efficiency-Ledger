// Fabric Efficiency Ledger - shared benchmark scaffolding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every benchmark reports completed work rather than a bare elapsed time: the
// number of records, cells or bytes that were actually produced and verified.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fel/ledger.hpp"

namespace bench {

using namespace fel;

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] double seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

inline void report(const char* name, std::uint64_t completed, const char* unit, double seconds) {
  const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
  std::printf("%-28s completed=%llu %-10s elapsed=%.4fs rate=%.0f per second\n", name,
              static_cast<unsigned long long>(completed), unit, seconds, rate);
}

// A representative fabric: N ports under one switch, all bound to generation 1.
struct BenchFixture {
  FabricTopology topology;
  GenerationId generation;
  SourceId source;
  IncarnationId incarnation;
  ScopeId fabric_scope;
  ScopeId switch_scope;
  std::vector<ResourceId> ports;
  TimePoint start;
  TimePoint end;
};

[[nodiscard]] inline BenchFixture make_fixture(std::size_t port_count, TimePoint start,
                                               Duration span) {
  BenchFixture fixture;
  fixture.start = start;
  fixture.end = start + span;
  fixture.generation = GenerationId::derive({"bench", "generation"});
  fixture.source = SourceId::derive({"bench", "source"});
  fixture.incarnation = IncarnationId::derive({"bench", "incarnation"});
  fixture.fabric_scope = ScopeId::derive({"bench", "fabric"});
  fixture.switch_scope = ScopeId::derive({"bench", "switch"});

  Generation generation;
  generation.id = fixture.generation;
  generation.ordinal = Ordinal{1};
  generation.label = "bench";
  generation.effective_from = start - Duration::from_days(1);
  static_cast<void>(fixture.topology.add_generation(generation));

  Source source;
  source.id = fixture.source;
  source.label = "bench source";
  source.kind = SourceKind::PortCounter;
  source.authority = AuthorityRank{10};
  source.provenance = ProvenanceClass::Real;
  static_cast<void>(fixture.topology.add_source(source));

  SourceIncarnation incarnation;
  incarnation.id = fixture.incarnation;
  incarnation.source = fixture.source;
  incarnation.boot_epoch = EpochId{1};
  incarnation.started_at = start - Duration::from_days(1);
  static_cast<void>(fixture.topology.add_incarnation(incarnation));

  Scope fabric;
  fabric.id = fixture.fabric_scope;
  fabric.label = "fabric";
  fabric.kind = ScopeKind::Fabric;
  static_cast<void>(fixture.topology.add_scope(fabric));

  Scope parent;
  parent.id = fixture.switch_scope;
  parent.label = "switch";
  parent.kind = ScopeKind::Switch;
  parent.parent = fixture.fabric_scope;
  static_cast<void>(fixture.topology.add_scope(parent));

  for (std::size_t i = 0; i < port_count; ++i) {
    const std::string label = "port-" + std::to_string(i);
    const ScopeId scope = ScopeId::derive({"bench", "scope", label});
    const ResourceId resource = ResourceId::derive({"bench", "resource", label});
    fixture.ports.push_back(resource);

    Scope leaf;
    leaf.id = scope;
    leaf.label = label;
    leaf.kind = ScopeKind::Port;
    leaf.parent = fixture.switch_scope;
    leaf.members = {resource};
    static_cast<void>(fixture.topology.add_scope(leaf));

    Resource port;
    port.id = resource;
    port.label = label;
    port.kind = ResourceKind::Port;
    port.scope = scope;
    static_cast<void>(fixture.topology.add_resource(port));

    GenerationBinding binding;
    binding.id = BindingId::derive({"bench", "binding", label});
    binding.resource = resource;
    binding.generation = fixture.generation;
    binding.valid_from = start - Duration::from_days(1);
    static_cast<void>(fixture.topology.add_binding(binding));
  }
  return fixture;
}

// Deterministic evidence spread across the period, cycling through windows,
// ports and categories.
//
// The amount is a function of the accounting cell and the reporting window
// only. Two records that describe the same cell and the same window therefore
// agree by construction: a benchmark that manufactured disagreements would be
// measuring conflict resolution rather than accounting throughput.
[[nodiscard]] inline std::vector<Observation> make_evidence(const BenchFixture& fixture,
                                                            std::size_t count,
                                                            std::int64_t window_count) {
  std::vector<Observation> observations;
  observations.reserve(count);
  const std::int64_t span_minutes = (fixture.end - fixture.start).seconds() / 60;
  const std::int64_t width = span_minutes / window_count;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t slot = i % static_cast<std::size_t>(window_count);
    const std::size_t port_index = i % fixture.ports.size();
    const std::size_t category_index = i % kCategoryCount;
    Observation observation;
    observation.source = fixture.source;
    observation.incarnation = fixture.incarnation;
    observation.epoch = EpochId{1};
    observation.sequence = SourceSequence{i + 1};
    observation.generation = fixture.generation;
    observation.window_start =
        fixture.start + Duration::from_minutes(static_cast<std::int64_t>(slot) * width);
    observation.window_end = observation.window_start + Duration::from_minutes(width);
    observation.observed_at = observation.window_end;
    observation.received_at = observation.window_end;
    observation.kind = MeasureKind::WireBytes;
    observation.category = static_cast<Category>(1 + category_index);
    observation.resource = fixture.ports[port_index];
    observation.amount = 1000 + ((slot * 7 + port_index * 13 + category_index * 29) % 997);
    observation.origin = OriginId::derive({"bench", std::to_string(i)});
    observation.id = EvidenceId::derive({"bench", std::to_string(i)});
    observations.push_back(observation);
  }
  return observations;
}

[[nodiscard]] inline ObservationBatch make_batch_for(const std::vector<Observation>& evidence) {
  ObservationBatch batch;
  batch.observations = evidence;
  batch.origin_label = "benchmark";
  return batch;
}

[[nodiscard]] inline LedgerPolicy bench_policy() {
  LedgerPolicy policy;
  policy.freshness.fresh_ttl = Duration::from_days(2);
  policy.freshness.stale_ttl = Duration::from_days(4);
  policy.workers = 4;
  policy.queue_depth = 1024;
  return policy;
}

}  // namespace bench
