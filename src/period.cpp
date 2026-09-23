// Fabric Efficiency Ledger - accounting periods, revisions, corrections.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/period.hpp"

#include <algorithm>
#include <string>

#include "fel/serialize.hpp"

namespace fel {

std::string_view period_state_name(PeriodState state) noexcept {
  switch (state) {
    case PeriodState::Open:
      return "open";
    case PeriodState::Closed:
      return "closed";
    case PeriodState::Superseded:
      return "superseded";
  }
  return "invalid";
}

std::string_view conservation_status_name(ConservationStatus status) noexcept {
  switch (status) {
    case ConservationStatus::Consistent:
      return "consistent";
    case ConservationStatus::Residual:
      return "residual";
    case ConservationStatus::Excess:
      return "excess";
    case ConservationStatus::Unverifiable:
      return "unverifiable";
  }
  return "invalid";
}

std::string AccountingPeriod::canonical_form() const {
  FieldWriter writer;
  writer.field("period/1");
  writer.field(id.to_string());
  writer.field_u64(ordinal.value());
  writer.field(label);
  writer.field_i64(start.nanos());
  writer.field_i64(end.nanos());
  writer.field(period_state_name(state));
  writer.field_u64(current_revision.value());
  writer.field_u64(observations_ingested);
  return writer.text();
}

std::string PeriodRevision::canonical_form() const {
  FieldWriter writer;
  writer.field("period-revision/2");
  writer.field(period.to_string());
  writer.field_u64(revision.value());
  writer.field(parent_digest.has_value() ? parent_digest->to_hex() : std::string("-"));
  writer.field(correction.has_value() ? correction->to_string() : std::string("-"));
  writer.field(correction_reason);
  writer.field_i64(closed_at.nanos());
  writer.field(policy_revision.to_string());
  writer.field(topology_revision.to_string());
  writer.field(store.to_string());

  writer.field_u64(static_cast<std::uint64_t>(generations.size()));
  for (const auto& value : generations) {
    writer.field(value.to_string());
  }

  writer.field_u64(static_cast<std::uint64_t>(claims.size()));
  for (const auto& claim : claims) {
    writer.field(claim.canonical_form());
  }

  writer.field_u64(static_cast<std::uint64_t>(unknowns.size()));
  for (const auto& entry : unknowns) {
    writer.field(entry.scope.to_string());
    writer.field(entry.generation.to_string());
    writer.field(measure_kind_name(entry.kind));
    writer.field(unknown_reason_name(entry.reason));
    writer.field_u64(entry.evidence_count);
    writer.field(entry.note);
  }

  writer.field_u64(static_cast<std::uint64_t>(conflicts.size()));
  for (const auto& conflict : conflicts) {
    writer.field(conflict.identity.to_string());
    writer.field(conflict.key.canonical_form());
    writer.field_u64(conflict.distinct_amounts);
    writer.field_bool(conflict.resolved);
    writer.field(conflict.resolution);
    writer.field(conflict.note);
    for (const auto& participant : conflict.participants) {
      writer.field(participant.to_string());
    }
    for (const auto& source : conflict.sources) {
      writer.field(source.to_string());
    }
  }

  writer.field_u64(static_cast<std::uint64_t>(rejected.size()));
  for (const auto& entry : rejected) {
    writer.field(entry.evidence.to_string());
    writer.field(entry.source.to_string());
    writer.field(measure_kind_name(entry.kind));
    writer.field(unknown_reason_name(entry.reason));
    writer.field(entry.detail);
  }

  writer.field_u64(static_cast<std::uint64_t>(conservation.size()));
  for (const auto& domain : conservation) {
    writer.field(domain.scope.to_string());
    writer.field(domain.generation.to_string());
    writer.field(measure_kind_name(domain.kind));
    writer.field_u64(domain.attributed_total);
    writer.field_u64(domain.unknown_contributions);
    writer.field_u64(domain.contribution_count);
    writer.field(domain.observed_total.has_value()
                     ? std::to_string(*domain.observed_total)
                     : std::string("-"));
    writer.field_u64(domain.total_observations);
    writer.field_i64(domain.delta);
    writer.field_u64(domain.residual_to_unknown);
    writer.field(conservation_status_name(domain.status));
    for (const auto& evidence : domain.total_evidence) {
      writer.field(evidence.to_string());
    }
  }

  writer.field_u64(static_cast<std::uint64_t>(efficiency.size()));
  for (const auto& summary : efficiency) {
    writer.field(measure_kind_name(summary.kind));
    writer.field(coverage_name(summary.coverage));
    writer.field_bool(summary.determinate);
    writer.field(summary.indeterminacy_reason);
    writer.field_u64(summary.useful);
    writer.field_u64(summary.necessary_overhead);
    writer.field_u64(summary.avoidable);
    writer.field_u64(summary.failed);
    writer.field_u64(summary.unknown_contributions);
    writer.field_u64(summary.denominator);
    writer.field_u64(summary.ratio_numerator);
    writer.field_u64(summary.ratio_denominator);
    writer.field_u64(summary.ratio_permille);
  }

  writer.field_u64(static_cast<std::uint64_t>(proof_surfaces.size()));
  for (const auto value : proof_surfaces) {
    writer.field(provenance_class_name(value));
  }

  writer.field_u64(evidence_considered);
  writer.field_u64(evidence_admitted);
  writer.field_u64(evidence_rejected);
  writer.field_u64(duplicate_suppressed);
  writer.field_bool(stale_included);
  writer.field_bool(has_residual);
  writer.field_bool(has_excess);
  writer.field_bool(has_conflicts);
  writer.field_bool(has_unknown);
  writer.field_bool(complete);
  return writer.text();
}

Digest256 PeriodRevision::recompute_digest() const { return Sha256::hash(canonical_form()); }

Result<void> PeriodRevision::verify_digest() const {
  const Digest256 actual = recompute_digest();
  if (actual != content_digest) {
    return make_error(ErrorCode::RecordChecksumMismatch,
                      "period revision content digest does not match its content",
                      period.to_string() + "#" + revision.to_string());
  }
  return ok();
}

std::string CorrectionRecord::canonical_form() const {
  FieldWriter writer;
  writer.field("correction/1");
  writer.field(id.to_string());
  writer.field(period.to_string());
  writer.field_u64(from_revision.value());
  writer.field_u64(to_revision.value());
  writer.field_i64(created_at.nanos());
  writer.field(reason);
  writer.field(operator_label);
  writer.field(parent_digest.to_hex());
  writer.field(new_digest.to_hex());
  writer.field(previous_correction.has_value() ? previous_correction->to_hex()
                                               : std::string("-"));
  for (const auto& evidence : added_evidence) {
    writer.field(evidence.to_string());
  }
  return writer.text();
}

}  // namespace fel
