// Fabric Efficiency Ledger - deterministic test fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "support/fixtures.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <system_error>

namespace feltest {
namespace {

[[nodiscard]] fel::TimePoint at(std::int64_t seconds) {
  return fel::TimePoint::from_seconds(seconds);
}

}  // namespace

Fixture make_fixture() {
  Fixture fixture;
  fixture.epoch0 = at(1700000000);
  fixture.period_start = at(1700000000);
  fixture.period_end = at(1700003600);

  fixture.generation = fel::GenerationId::derive({"generation", "g1"});
  fixture.superseded_generation = fel::GenerationId::derive({"generation", "g0"});

  fel::Generation active;
  active.id = fixture.generation;
  active.ordinal = fel::Ordinal{2};
  active.label = "generation-1";
  active.effective_from = fixture.epoch0;
  active.state = fel::GenerationState::Active;
  (void)fixture.topology.add_generation(active);

  fel::Generation superseded;
  superseded.id = fixture.superseded_generation;
  superseded.ordinal = fel::Ordinal{1};
  superseded.label = "generation-0";
  superseded.effective_from = at(1690000000);
  superseded.effective_to = fixture.epoch0;
  superseded.state = fel::GenerationState::Superseded;
  (void)fixture.topology.add_generation(superseded);

  fixture.counter_source = fel::SourceId::derive({"source", "switch-a"});
  fixture.probe_source = fel::SourceId::derive({"source", "probe-b"});
  fixture.unsupported_source = fel::SourceId::derive({"source", "vendor-x"});

  fel::Source counter;
  counter.id = fixture.counter_source;
  counter.label = "switch-a counters";
  counter.kind = fel::SourceKind::SwitchCounter;
  counter.authority = fel::AuthorityRank{10};
  counter.provenance = fel::ProvenanceClass::Real;
  (void)fixture.topology.add_source(counter);

  fel::Source probe;
  probe.id = fixture.probe_source;
  probe.label = "probe-b sampler";
  probe.kind = fel::SourceKind::FlowSampler;
  probe.authority = fel::AuthorityRank{5};
  probe.provenance = fel::ProvenanceClass::Synthetic;
  (void)fixture.topology.add_source(probe);

  fel::Source unsupported;
  unsupported.id = fixture.unsupported_source;
  unsupported.label = "vendor-x proprietary counter";
  unsupported.kind = fel::SourceKind::SwitchCounter;
  unsupported.authority = fel::AuthorityRank{100};
  unsupported.provenance = fel::ProvenanceClass::Unsupported;
  (void)fixture.topology.add_source(unsupported);

  fixture.counter_incarnation = fel::IncarnationId::derive({"incarnation", "switch-a", "boot-1"});
  fel::SourceIncarnation counter_incarnation;
  counter_incarnation.id = fixture.counter_incarnation;
  counter_incarnation.source = fixture.counter_source;
  counter_incarnation.boot_epoch = fel::EpochId{1};
  counter_incarnation.started_at = at(1699990000);
  (void)fixture.topology.add_incarnation(counter_incarnation);

  fixture.probe_incarnation = fel::IncarnationId::derive({"incarnation", "probe-b", "boot-1"});
  fel::SourceIncarnation probe_incarnation;
  probe_incarnation.id = fixture.probe_incarnation;
  probe_incarnation.source = fixture.probe_source;
  probe_incarnation.boot_epoch = fel::EpochId{1};
  probe_incarnation.started_at = at(1699990000);
  (void)fixture.topology.add_incarnation(probe_incarnation);

  fixture.fabric_scope = fel::ScopeId::derive({"scope", "fabric"});
  fixture.pod_scope = fel::ScopeId::derive({"scope", "pod-1"});
  fixture.switch_scope = fel::ScopeId::derive({"scope", "switch-1"});
  fixture.port1_scope = fel::ScopeId::derive({"scope", "port-1"});
  fixture.port2_scope = fel::ScopeId::derive({"scope", "port-2"});

  fel::Scope fabric;
  fabric.id = fixture.fabric_scope;
  fabric.label = "fabric";
  fabric.kind = fel::ScopeKind::Fabric;
  (void)fixture.topology.add_scope(fabric);

  fel::Scope pod;
  pod.id = fixture.pod_scope;
  pod.label = "pod-1";
  pod.kind = fel::ScopeKind::Pod;
  pod.parent = fixture.fabric_scope;
  (void)fixture.topology.add_scope(pod);

  fel::Scope sw;
  sw.id = fixture.switch_scope;
  sw.label = "switch-1";
  sw.kind = fel::ScopeKind::Switch;
  sw.parent = fixture.pod_scope;
  (void)fixture.topology.add_scope(sw);

  fixture.port1 = fel::ResourceId::derive({"resource", "port-1"});
  fixture.port2 = fel::ResourceId::derive({"resource", "port-2"});

  fel::Scope p1;
  p1.id = fixture.port1_scope;
  p1.label = "port-1";
  p1.kind = fel::ScopeKind::Port;
  p1.parent = fixture.switch_scope;
  p1.members = {fixture.port1};
  (void)fixture.topology.add_scope(p1);

  fel::Scope p2;
  p2.id = fixture.port2_scope;
  p2.label = "port-2";
  p2.kind = fel::ScopeKind::Port;
  p2.parent = fixture.switch_scope;
  p2.members = {fixture.port2};
  (void)fixture.topology.add_scope(p2);

  fel::Resource port1;
  port1.id = fixture.port1;
  port1.label = "port-1";
  port1.kind = fel::ResourceKind::Port;
  port1.scope = fixture.port1_scope;
  port1.locality = "switch-1";
  (void)fixture.topology.add_resource(port1);

  fel::Resource port2;
  port2.id = fixture.port2;
  port2.label = "port-2";
  port2.kind = fel::ResourceKind::Port;
  port2.scope = fixture.port2_scope;
  port2.locality = "switch-1";
  (void)fixture.topology.add_resource(port2);

  for (const auto& resource : {fixture.port1, fixture.port2}) {
    fel::GenerationBinding binding;
    binding.resource = resource;
    binding.generation = fixture.generation;
    binding.valid_from = at(1699990000);
    binding.id = fel::BindingId::derive({resource.to_string(), fixture.generation.to_string(), "1"});
    (void)fixture.topology.add_binding(binding);
  }

  fixture.flow1 = fel::FlowId::derive({"flow", "tenant-a"});
  fel::Flow flow;
  flow.id = fixture.flow1;
  flow.label = "tenant-a";
  flow.scope = fixture.switch_scope;
  flow.generation = fixture.generation;
  flow.path_hint = {fixture.port1, fixture.port2};
  (void)fixture.topology.add_flow(flow);

  fixture.path1 = fel::PathId::derive({"path", "p1"});
  fel::Path path;
  path.id = fixture.path1;
  path.label = "path-1";
  path.scope = fixture.switch_scope;
  path.generation = fixture.generation;
  path.hops = {fixture.port1, fixture.port2};
  (void)fixture.topology.add_path(path);

  fixture.reservation1 = fel::ReservationId::derive({"reservation", "res-1"});
  fel::Reservation reservation;
  reservation.id = fixture.reservation1;
  reservation.label = "reservation-1";
  reservation.scope = fixture.switch_scope;
  reservation.generation = fixture.generation;
  reservation.resource = fixture.port2;
  reservation.reserved_capacity_nanos = 1000000000ULL;
  reservation.valid_from = at(1699990000);
  (void)fixture.topology.add_reservation(reservation);

  return fixture;
}

fel::Observation make_observation(const ObservationSpec& spec) {
  fel::Observation observation;
  observation.source = spec.source;
  observation.incarnation = spec.incarnation;
  observation.epoch = fel::EpochId{spec.epoch};
  observation.sequence = fel::SourceSequence{spec.sequence};
  observation.generation = spec.generation;
  observation.observed_at = spec.observed_at;
  observation.received_at = spec.received_at;
  observation.kind = spec.kind;
  observation.role = spec.role;
  observation.category = spec.category;
  observation.resource = spec.resource;
  observation.flow = spec.flow;
  observation.path = spec.path;
  observation.reservation = spec.reservation;
  observation.amount = spec.amount;
  observation.window_start = spec.window_start;
  observation.window_end = spec.window_end;
  observation.origin = spec.origin.value_or(fel::OriginId::derive(
      {spec.source.to_string(), std::to_string(spec.sequence), spec.label}));
  observation.id = fel::EvidenceId::derive({observation.origin.to_string(),
                                            spec.source.to_string(),
                                            std::to_string(spec.sequence),
                                            std::to_string(spec.amount),
                                            spec.category == fel::Category::UsefulDeliveredWork
                                                ? std::string("a")
                                                : std::string("b")});
  return observation;
}

fel::LedgerPolicy test_policy() {
  fel::LedgerPolicy policy;
  policy.freshness.fresh_ttl = fel::Duration::from_hours(24);
  policy.freshness.stale_ttl = fel::Duration::from_hours(48);
  policy.freshness.clock_skew_allowance = fel::Duration::from_seconds(30);
  policy.workers = 4;
  policy.queue_depth = 256;
  return policy;
}

fel::Result<LedgerRig> make_memory_rig() { return make_memory_rig_with(test_policy()); }

fel::Result<LedgerRig> make_memory_rig_with(fel::LedgerPolicy policy) {
  LedgerRig rig;
  rig.fixture = make_fixture();
  auto ledger = fel::Ledger::in_memory(policy, rig.fixture.topology, rig.fixture.period_end);
  if (!ledger.has_value()) {
    return ledger.error();
  }
  rig.ledger = std::move(ledger.value());
  auto period =
      rig.ledger->open_period(rig.fixture.period_start, rig.fixture.period_end, "period-1");
  if (!period.has_value()) {
    return period.error();
  }
  rig.period = period.value();
  return rig;
}

ObservationSpec make_spec(const Fixture& fixture, fel::SourceId source,
                          fel::IncarnationId incarnation, std::uint64_t sequence,
                          std::uint64_t amount, fel::Category category,
                          std::optional<fel::ResourceId> resource) {
  ObservationSpec spec;
  spec.source = source;
  spec.incarnation = incarnation;
  spec.sequence = sequence;
  spec.generation = fixture.generation;
  spec.amount = amount;
  spec.category = category;
  spec.resource = resource;
  spec.observed_at = fixture.period_start + fel::Duration::from_minutes(5);
  spec.received_at = spec.observed_at + fel::Duration::from_seconds(1);
  spec.window_start = fixture.period_start;
  spec.window_end = fixture.period_start + fel::Duration::from_minutes(10);
  spec.label = "spec-" + std::to_string(sequence);
  return spec;
}

fel::ObservationBatch make_batch(const std::vector<fel::Observation>& observations) {
  fel::ObservationBatch batch;
  batch.observations = observations;
  batch.format_version = fel::kEvidenceFormatVersion;
  batch.origin_label = "fixture";
  std::string text;
  for (const auto& observation : observations) {
    text += observation.canonical_form();
  }
  batch.document_digest = fel::Sha256::hash(text);
  return batch;
}

ScopedTempDir::ScopedTempDir(const std::string& tag) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  const std::string name = "fel-test-" + tag + "-" + std::to_string(now) + "-" +
                           std::to_string(serial);
  path_ = (base / name).string();
  std::filesystem::create_directories(path_, ec);
}

ScopedTempDir::~ScopedTempDir() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

std::string ScopedTempDir::child(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

}  // namespace feltest
