// Fabric Efficiency Ledger - result provenance stamp.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/provenance.hpp"

#include <string>

#include "fel/serialize.hpp"

namespace fel {

bool ProvenanceStamp::purely_real() const noexcept {
  return proof_surfaces.size() == 1 && proof_surfaces.front() == ProvenanceClass::Real;
}

std::string ProvenanceStamp::proof_surface_label() const {
  if (proof_surfaces.empty()) {
    return "NONE";
  }
  std::string out;
  for (const auto value : proof_surfaces) {
    if (!out.empty()) {
      out.push_back('+');
    }
    out += std::string(provenance_class_name(value));
  }
  return out;
}

std::string ProvenanceStamp::canonical_form() const {
  FieldWriter writer;
  writer.field("provenance-stamp/1");
  writer.field(policy_revision.to_string());
  writer.field(policy_digest.to_hex());
  writer.field(topology_revision.to_string());
  writer.field(topology_digest.to_hex());
  writer.field(store.to_string());
  writer.field_u64(store_revision);
  writer.field_i64(as_of.nanos());
  writer.field_i64(store_watermark.nanos());
  for (const auto& value : generations) {
    writer.field(value.to_string());
  }
  for (const auto value : proof_surfaces) {
    writer.field(provenance_class_name(value));
  }
  writer.field_bool(freshness_rebased);
  writer.field_bool(contains_stale);
  writer.field_bool(contains_unknown);
  return writer.text();
}

}  // namespace fel
