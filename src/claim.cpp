// Fabric Efficiency Ledger - accounting cells, claims, and conflicts.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/claim.hpp"

#include <string>

#include "fel/serialize.hpp"

namespace fel {
namespace {

constexpr const char* kBasisNames[] = {"resource-direct", "flow-direct",   "path-direct",
                                       "reservation-direct", "scope-declared", "unattributed"};

}  // namespace

std::string_view attribution_basis_name(AttributionBasis basis) noexcept {
  const auto index = static_cast<std::size_t>(basis);
  if (index == 0 || index > kAttributionBasisCount) {
    return "invalid";
  }
  return kBasisNames[index - 1];
}

bool operator==(const ClaimKey& a, const ClaimKey& b) noexcept {
  return a.generation == b.generation && a.scope == b.scope && a.kind == b.kind &&
         a.category == b.category && a.basis == b.basis && a.resource == b.resource &&
         a.flow == b.flow && a.path == b.path && a.reservation == b.reservation;
}

bool operator<(const ClaimKey& a, const ClaimKey& b) noexcept {
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.scope != b.scope) return a.scope < b.scope;
  if (a.kind != b.kind) return a.kind < b.kind;
  if (a.category != b.category) return a.category < b.category;
  if (a.basis != b.basis) return a.basis < b.basis;
  if (a.resource != b.resource) return a.resource < b.resource;
  if (a.flow != b.flow) return a.flow < b.flow;
  if (a.path != b.path) return a.path < b.path;
  return a.reservation < b.reservation;
}

std::string ClaimKey::canonical_form() const {
  FieldWriter writer;
  writer.field("claim-key/1");
  writer.field(generation.to_string());
  writer.field(scope.to_string());
  writer.field(measure_kind_name(kind));
  writer.field(category_name(category));
  writer.field(attribution_basis_name(basis));
  writer.field(resource.to_string());
  writer.field(flow.to_string());
  writer.field(path.to_string());
  writer.field(reservation.to_string());
  return writer.text();
}

AccountingIdentity ClaimKey::identity() const {
  FieldWriter writer;
  writer.field("fel.accounting-identity/1");
  writer.field(generation.to_string());
  writer.field(scope.to_string());
  writer.field(measure_kind_name(kind));
  writer.field(category_name(category));
  writer.field(attribution_basis_name(basis));
  writer.field(resource.to_string());
  writer.field(flow.to_string());
  writer.field(path.to_string());
  writer.field(reservation.to_string());
  return AccountingIdentity::from_digest(writer.digest());
}

std::string Claim::canonical_form() const {
  FieldWriter writer;
  writer.field("claim/1");
  writer.field(id.to_string());
  writer.field(key.canonical_form());
  writer.field(period.to_string());
  writer.field_u64(cell.known_total);
  writer.field_u64(cell.known_contributions);
  writer.field_u64(cell.unknown_contributions);
  writer.field_bool(has_conflict);
  writer.field_bool(stale_included);
  for (const auto& value : evidence) {
    writer.field(value.to_string());
  }
  for (const auto& value : sources) {
    writer.field(value.to_string());
  }
  for (const auto& value : provenance) {
    writer.field(provenance_class_name(value));
  }
  for (const auto& value : unknowns) {
    writer.field(value.scope.to_string());
    writer.field(value.generation.to_string());
    writer.field(measure_kind_name(value.kind));
    writer.field(unknown_reason_name(value.reason));
    writer.field_u64(value.evidence_count);
    writer.field(value.note);
  }
  return writer.text();
}

}  // namespace fel
