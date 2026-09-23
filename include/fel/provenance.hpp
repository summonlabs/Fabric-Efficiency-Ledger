// Fabric Efficiency Ledger - result provenance stamp.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string>
#include <vector>

#include "fel/crypto.hpp"
#include "fel/id.hpp"
#include "fel/topology.hpp"

namespace fel {

// Every result the ledger produces carries this stamp. It answers, without
// ambiguity: which policy revision, which topology revision, which "as of"
// instant, which store revision, which fabric generations, and which provenance
// classes contributed. A consumer can therefore always distinguish REAL from
// SYNTHETIC or REPLAY input, and see when a claim was UNSUPPORTED.
struct ProvenanceStamp {
  PolicyRevisionId policy_revision;
  Digest256 policy_digest;
  TopologyRevisionId topology_revision;
  Digest256 topology_digest;
  StoreId store;
  std::uint64_t store_revision = 0;
  TimePoint as_of;
  TimePoint store_watermark;
  std::vector<GenerationId> generations;
  std::vector<ProvenanceClass> proof_surfaces;
  bool freshness_rebased = false;
  bool contains_stale = false;
  bool contains_unknown = false;

  [[nodiscard]] bool purely_real() const noexcept;
  [[nodiscard]] std::string proof_surface_label() const;
  [[nodiscard]] std::string canonical_form() const;
};

}  // namespace fel
