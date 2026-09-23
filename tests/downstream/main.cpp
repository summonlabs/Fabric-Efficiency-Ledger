// Fabric Efficiency Ledger - independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Uses only the installed public headers and the exported fel::fel target. It
// builds a minimal fabric through the public API, accounts one period of
// evidence, closes it, and checks the closed revision digest is reproducible.
#include <cstdio>
#include <string>
#include <vector>

#include "fel/io.hpp"
#include "fel/ledger.hpp"
#include "fel/version.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace fel;

  std::printf("linked against Fabric Efficiency Ledger %s\n",
              std::string(kVersionString).c_str());

  const TimePoint start = TimePoint::from_seconds(1700000000);
  const TimePoint end = start + Duration::from_hours(1);
  const GenerationId generation = GenerationId::derive({"downstream", "generation"});
  const SourceId source = SourceId::derive({"downstream", "source"});
  const IncarnationId incarnation = IncarnationId::derive({"downstream", "incarnation"});
  const ScopeId fabric = ScopeId::derive({"downstream", "fabric"});
  const ScopeId port_scope = ScopeId::derive({"downstream", "port"});
  const ResourceId port = ResourceId::derive({"downstream", "resource"});

  FabricTopology topology;
  Generation active;
  active.id = generation;
  active.ordinal = Ordinal{1};
  active.label = "g1";
  active.effective_from = start - Duration::from_days(1);
  check(topology.add_generation(active).has_value(), "add generation");
  Source agent;
  agent.id = source;
  agent.label = "source";
  agent.kind = SourceKind::PortCounter;
  agent.authority = AuthorityRank{5};
  agent.provenance = ProvenanceClass::Synthetic;
  check(topology.add_source(agent).has_value(), "add source");
  SourceIncarnation boot;
  boot.id = incarnation;
  boot.source = source;
  boot.boot_epoch = EpochId{1};
  boot.started_at = start - Duration::from_days(1);
  check(topology.add_incarnation(boot).has_value(), "add incarnation");
  Scope root;
  root.id = fabric;
  root.label = "fabric";
  root.kind = ScopeKind::Fabric;
  check(topology.add_scope(root).has_value(), "add fabric scope");
  Scope leaf;
  leaf.id = port_scope;
  leaf.label = "port";
  leaf.kind = ScopeKind::Port;
  leaf.parent = fabric;
  leaf.members = {port};
  check(topology.add_scope(leaf).has_value(), "add port scope");
  Resource resource;
  resource.id = port;
  resource.label = "port";
  resource.kind = ResourceKind::Port;
  resource.scope = port_scope;
  check(topology.add_resource(resource).has_value(), "add resource");
  GenerationBinding binding;
  binding.id = BindingId::derive({"downstream", "binding"});
  binding.resource = port;
  binding.generation = generation;
  binding.valid_from = start - Duration::from_days(1);
  check(topology.add_binding(binding).has_value(), "add binding");
  check(topology.validate().has_value(), "topology validates");

  LedgerPolicy policy;
  policy.freshness.fresh_ttl = Duration::from_hours(24);
  policy.freshness.stale_ttl = Duration::from_hours(48);

  auto ledger = Ledger::in_memory(policy, topology, end);
  check(ledger.has_value(), "ledger open");
  if (!ledger.has_value()) {
    return 1;
  }
  auto period = ledger.value()->open_period(start, end, "downstream");
  check(period.has_value(), "period open");
  if (!period.has_value()) {
    return 1;
  }

  Observation observation;
  observation.source = source;
  observation.incarnation = incarnation;
  observation.epoch = EpochId{1};
  observation.sequence = SourceSequence{1};
  observation.generation = generation;
  observation.observed_at = start + Duration::from_minutes(5);
  observation.received_at = observation.observed_at;
  observation.kind = MeasureKind::WireBytes;
  observation.category = Category::UsefulDeliveredWork;
  observation.resource = port;
  observation.amount = 4096;
  observation.window_start = start;
  observation.window_end = start + Duration::from_minutes(10);
  observation.origin = OriginId::derive({"downstream", "1"});
  observation.id = EvidenceId::derive({"downstream", "1"});

  // Round trip the observation through the public document codecs, which is how
  // an operator would feed the runtime from a file.
  const std::string document = observation_to_json(observation).dump(0);
  auto parsed = parse_evidence_document(document + "\n", "downstream");
  check(parsed.has_value(), "evidence document parses");
  if (!parsed.has_value()) {
    return 1;
  }

  auto report = ledger.value()->ingest(parsed.value());
  check(report.has_value() && report.value().accepted == 1, "evidence accepted");

  auto closed = ledger.value()->close_period(
      CloseRequest{period.value(), end, "downstream close", "downstream", false});
  check(closed.has_value(), "period closes");
  if (!closed.has_value()) {
    return 1;
  }
  std::uint64_t total = 0;
  for (const auto& claim : closed.value().claims) {
    total += claim.cell.known_total;
  }
  check(total == 4096, "accounted total is exact");

  // Closing is a pure function of evidence and policy: re-deriving reproduces
  // the identical content digest.
  auto again = ledger.value()->derive(period.value(), end, RevisionOrdinal{1}, std::nullopt,
                                      std::nullopt, std::string{"downstream close"},
                                      std::string{"downstream"});
  check(again.has_value(), "re-derivation succeeds");
  if (again.has_value()) {
    check(again.value().content_digest.to_hex() == closed.value().content_digest.to_hex(),
          "re-derivation reproduces the content digest");
  }

  auto integrity = ledger.value()->verify_integrity();
  check(integrity.has_value() && integrity.value().ok, "integrity check passes");
  check(closed.value().proof_surfaces.size() == 1 &&
            closed.value().proof_surfaces.front() == ProvenanceClass::Synthetic,
        "proof surface is labelled synthetic");

  std::printf("downstream consumer failures: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
