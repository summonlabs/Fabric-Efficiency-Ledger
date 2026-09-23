// Fabric Efficiency Ledger - JSON codecs for persisted ledger records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "ledger_json.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "fel/limits.hpp"
#include "fel/version.hpp"

namespace fel {
namespace detail {
namespace {

[[nodiscard]] Json time_or_dash(TimePoint value) {
  return Json::string(value.is_zero() ? std::string("-") : value.to_rfc3339_millis());
}

[[nodiscard]] Result<TimePoint> time_from_json(const Json& document, const char* key) {
  FEL_TRY_ASSIGN(const std::string text, document.require_string(key));
  if (text == "-") {
    return TimePoint{};
  }
  return parse_rfc3339(text);
}

template <class Id>
[[nodiscard]] Result<Id> typed_id_from_json(const Json& document, const char* key) {
  FEL_TRY_ASSIGN(const std::string text, document.require_string(key));
  return Id::parse(text);
}

[[nodiscard]] Result<std::optional<ResourceId>> optional_resource(const Json& document,
                                                                  const char* key) {
  const Json* value = document.find(key);
  if (value == nullptr || !value->is_string() || value->as_string() == "-") {
    return std::optional<ResourceId>{};
  }
  FEL_TRY_ASSIGN(const ResourceId parsed, ResourceId::parse(value->as_string()));
  return std::optional<ResourceId>{parsed};
}

[[nodiscard]] Result<std::optional<FlowId>> optional_flow(const Json& document, const char* key) {
  const Json* value = document.find(key);
  if (value == nullptr || !value->is_string() || value->as_string() == "-") {
    return std::optional<FlowId>{};
  }
  FEL_TRY_ASSIGN(const FlowId parsed, FlowId::parse(value->as_string()));
  return std::optional<FlowId>{parsed};
}

[[nodiscard]] Result<std::optional<PathId>> optional_path(const Json& document, const char* key) {
  const Json* value = document.find(key);
  if (value == nullptr || !value->is_string() || value->as_string() == "-") {
    return std::optional<PathId>{};
  }
  FEL_TRY_ASSIGN(const PathId parsed, PathId::parse(value->as_string()));
  return std::optional<PathId>{parsed};
}

[[nodiscard]] Result<std::optional<ReservationId>> optional_reservation(const Json& document,
                                                                        const char* key) {
  const Json* value = document.find(key);
  if (value == nullptr || !value->is_string() || value->as_string() == "-") {
    return std::optional<ReservationId>{};
  }
  FEL_TRY_ASSIGN(const ReservationId parsed, ReservationId::parse(value->as_string()));
  return std::optional<ReservationId>{parsed};
}

[[nodiscard]] Result<ClaimKey> claim_key_from_json(const Json& document) {
  ClaimKey key;
  FEL_TRY_ASSIGN(key.generation, typed_id_from_json<GenerationId>(document, "generation"));
  const std::string scope_text = document.require_string("scope").value_or(std::string());
  if (!scope_text.empty() && scope_text != "-") {
    FEL_TRY_ASSIGN(key.scope, ScopeId::parse(scope_text));
  }
  FEL_TRY_ASSIGN(const std::string kind_text, document.require_string("measure"));
  const auto kind = measure_kind_from_name(kind_text);
  if (!kind.has_value()) {
    return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind", kind_text);
  }
  key.kind = kind.value();
  FEL_TRY_ASSIGN(const std::string category_text, document.require_string("category"));
  const auto category = category_from_name(category_text);
  if (!category.has_value()) {
    return make_error(ErrorCode::UnknownCategory, "unknown category", category_text);
  }
  key.category = category.value();
  FEL_TRY_ASSIGN(const std::string basis_text, document.require_string("basis"));
  bool basis_found = false;
  for (std::size_t i = 1; i <= kAttributionBasisCount; ++i) {
    if (basis_text == attribution_basis_name(static_cast<AttributionBasis>(i))) {
      key.basis = static_cast<AttributionBasis>(i);
      basis_found = true;
      break;
    }
  }
  if (!basis_found) {
    return make_error(ErrorCode::InvalidArgument, "unknown attribution basis", basis_text);
  }
  FEL_TRY_ASSIGN(key.resource, optional_resource(document, "resource").map(
                                   [](std::optional<ResourceId> value) {
                                     return value.value_or(ResourceId{});
                                   }));
  FEL_TRY_ASSIGN(key.flow, optional_flow(document, "flow").map([](std::optional<FlowId> value) {
                   return value.value_or(FlowId{});
                 }));
  FEL_TRY_ASSIGN(key.path, optional_path(document, "path").map([](std::optional<PathId> value) {
                   return value.value_or(PathId{});
                 }));
  FEL_TRY_ASSIGN(key.reservation,
                 optional_reservation(document, "reservation").map(
                     [](std::optional<ReservationId> value) {
                       return value.value_or(ReservationId{});
                     }));
  return key;
}

[[nodiscard]] Json claim_key_to_json(const ClaimKey& key) {
  Json out = Json::object();
  out.set("generation", Json::string(key.generation.to_string()));
  out.set("scope", Json::string(key.scope.is_nil() ? std::string("-") : key.scope.to_string()));
  out.set("measure", Json::string(std::string(measure_kind_name(key.kind))));
  out.set("category", Json::string(std::string(category_name(key.category))));
  out.set("basis", Json::string(std::string(attribution_basis_name(key.basis))));
  out.set("resource",
          Json::string(key.resource.is_nil() ? std::string("-") : key.resource.to_string()));
  out.set("flow", Json::string(key.flow.is_nil() ? std::string("-") : key.flow.to_string()));
  out.set("path", Json::string(key.path.is_nil() ? std::string("-") : key.path.to_string()));
  out.set("reservation",
          Json::string(key.reservation.is_nil() ? std::string("-") : key.reservation.to_string()));
  return out;
}

[[nodiscard]] Json unknown_to_json(const UnknownEntry& entry) {
  Json out = Json::object();
  out.set("scope", Json::string(entry.scope.is_nil() ? std::string("-") : entry.scope.to_string()));
  out.set("generation", Json::string(entry.generation.to_string()));
  out.set("measure", Json::string(std::string(measure_kind_name(entry.kind))));
  out.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
  out.set("evidence_count", Json::number(entry.evidence_count));
  if (!entry.note.empty()) {
    out.set("note", Json::string(entry.note));
  }
  return out;
}

[[nodiscard]] Result<UnknownEntry> unknown_from_json(const Json& document) {
  UnknownEntry entry;
  FEL_TRY_ASSIGN(const std::string scope_text, document.require_string("scope"));
  if (scope_text != "-") {
    FEL_TRY_ASSIGN(entry.scope, ScopeId::parse(scope_text));
  }
  FEL_TRY_ASSIGN(entry.generation, typed_id_from_json<GenerationId>(document, "generation"));
  FEL_TRY_ASSIGN(const std::string kind_text, document.require_string("measure"));
  const auto kind = measure_kind_from_name(kind_text);
  if (!kind.has_value()) {
    return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind", kind_text);
  }
  entry.kind = kind.value();
  FEL_TRY_ASSIGN(const std::string reason_text, document.require_string("reason"));
  const auto reason = unknown_reason_from_name(reason_text);
  if (!reason.has_value()) {
    return make_error(ErrorCode::InvalidArgument, "unknown unknown-reason", reason_text);
  }
  entry.reason = reason.value();
  FEL_TRY_ASSIGN(entry.evidence_count, document.require_u64("evidence_count"));
  if (const Json* note = document.find("note"); note != nullptr && note->is_string()) {
    entry.note = note->as_string();
  }
  return entry;
}

}  // namespace

Json observation_to_json(const Observation& observation) {
  Json out = Json::object();
  out.set("id", Json::string(observation.id.to_string()));
  out.set("origin", Json::string(observation.origin.to_string()));
  out.set("source", Json::string(observation.source.to_string()));
  out.set("incarnation", Json::string(observation.incarnation.to_string()));
  out.set("epoch", Json::number(observation.epoch.value()));
  out.set("sequence", Json::number(observation.sequence.value()));
  out.set("generation", Json::string(observation.generation.to_string()));
  out.set("observed_at", time_or_dash(observation.observed_at));
  out.set("received_at", time_or_dash(observation.received_at));
  out.set("measure", Json::string(std::string(measure_kind_name(observation.kind))));
  out.set("role", Json::string(std::string(observation_role_name(observation.role))));
  out.set("category", Json::string(std::string(category_name(observation.category))));
  out.set("resource", Json::string(observation.resource.has_value()
                                       ? observation.resource->to_string()
                                       : std::string("-")));
  out.set("flow",
          Json::string(observation.flow.has_value() ? observation.flow->to_string()
                                                    : std::string("-")));
  out.set("path",
          Json::string(observation.path.has_value() ? observation.path->to_string()
                                                    : std::string("-")));
  out.set("reservation", Json::string(observation.reservation.has_value()
                                          ? observation.reservation->to_string()
                                          : std::string("-")));
  out.set("amount", Json::number(observation.amount));
  out.set("window_start", time_or_dash(observation.window_start));
  out.set("window_end", time_or_dash(observation.window_end));
  Json metadata = Json::array();
  for (const auto& entry : observation.metadata) {
    Json item = Json::object();
    item.set("key", Json::string(entry.key));
    item.set("value", Json::string(entry.value));
    metadata.push(std::move(item));
  }
  out.set("metadata", std::move(metadata));
  if (observation.declared_integrity.has_value()) {
    out.set("integrity", Json::string(observation.declared_integrity->to_hex()));
  }
  return out;
}

Result<Observation> observation_from_json(const Json& document) {
  if (!document.is_object()) {
    return make_error(ErrorCode::SchemaViolation, "observation must be a JSON object");
  }
  FEL_TRY(document.reject_unknown_keys(
      {"id", "origin", "source", "incarnation", "epoch", "sequence", "generation", "observed_at",
       "received_at", "measure", "kind", "role", "category", "resource", "flow", "path",
       "reservation", "amount", "window_start", "window_end", "metadata", "integrity",
       "format_version"}));
  Observation observation;
  FEL_TRY_ASSIGN(const std::string id_text, document.require_string("id"));
  FEL_TRY_ASSIGN(observation.id, EvidenceId::parse(id_text));
  const Json* origin = document.find("origin");
  if (origin != nullptr && origin->is_string() && origin->as_string() != "-") {
    FEL_TRY_ASSIGN(observation.origin, OriginId::parse(origin->as_string()));
  } else {
    observation.origin = OriginId::from_raw(observation.id.raw());
  }
  FEL_TRY_ASSIGN(const std::string source_text, document.require_string("source"));
  FEL_TRY_ASSIGN(observation.source, SourceId::parse(source_text));
  FEL_TRY_ASSIGN(const std::string incarnation_text, document.require_string("incarnation"));
  FEL_TRY_ASSIGN(observation.incarnation, IncarnationId::parse(incarnation_text));
  std::uint64_t number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("epoch"));
  observation.epoch = EpochId{number};
  number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("sequence"));
  observation.sequence = SourceSequence{number};
  FEL_TRY_ASSIGN(const std::string generation_text, document.require_string("generation"));
  FEL_TRY_ASSIGN(observation.generation, GenerationId::parse(generation_text));
  FEL_TRY_ASSIGN(observation.observed_at, time_from_json(document, "observed_at"));
  FEL_TRY_ASSIGN(observation.received_at, time_from_json(document, "received_at"));

  const Json* measure = document.find("measure");
  const Json* legacy_kind = document.find("kind");
  if (measure == nullptr && legacy_kind == nullptr) {
    return make_error(ErrorCode::MissingRequiredField, "observation has no measure kind");
  }
  const Json& measure_value = measure != nullptr ? *measure : *legacy_kind;
  if (!measure_value.is_string()) {
    return make_error(ErrorCode::SchemaViolation, "measure must be a string");
  }
  const auto kind = measure_kind_from_name(measure_value.as_string());
  if (!kind.has_value()) {
    return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind",
                      measure_value.as_string());
  }
  observation.kind = kind.value();

  if (const Json* role = document.find("role"); role != nullptr) {
    if (!role->is_string()) {
      return make_error(ErrorCode::SchemaViolation, "role must be a string");
    }
    const auto parsed = observation_role_from_name(role->as_string());
    if (!parsed.has_value()) {
      return make_error(ErrorCode::InvalidArgument, "unknown observation role", role->as_string());
    }
    observation.role = parsed.value();
  }
  FEL_TRY_ASSIGN(const std::string category_text, document.require_string("category"));
  const auto category = category_from_name(category_text);
  if (!category.has_value()) {
    return make_error(ErrorCode::UnknownCategory, "unknown category", category_text);
  }
  observation.category = category.value();

  FEL_TRY_ASSIGN(observation.resource, optional_resource(document, "resource"));
  FEL_TRY_ASSIGN(observation.flow, optional_flow(document, "flow"));
  FEL_TRY_ASSIGN(observation.path, optional_path(document, "path"));
  FEL_TRY_ASSIGN(observation.reservation, optional_reservation(document, "reservation"));

  FEL_TRY_ASSIGN(observation.amount, document.require_u64("amount"));
  FEL_TRY_ASSIGN(observation.window_start, time_from_json(document, "window_start"));
  FEL_TRY_ASSIGN(observation.window_end, time_from_json(document, "window_end"));

  if (const Json* metadata = document.find("metadata"); metadata != nullptr) {
    if (!metadata->is_array()) {
      return make_error(ErrorCode::SchemaViolation, "metadata must be an array");
    }
    if (metadata->as_array().size() > limits::kMaxMetadataEntries) {
      return make_error(ErrorCode::MetadataTooLarge, "observation metadata exceeds the entry budget");
    }
    for (const auto& item : metadata->as_array()) {
      MetadataEntry entry;
      FEL_TRY_ASSIGN(entry.key, item.require_string("key"));
      FEL_TRY_ASSIGN(entry.value, item.require_string("value"));
      observation.metadata.push_back(std::move(entry));
    }
  }
  if (const Json* integrity = document.find("integrity"); integrity != nullptr) {
    if (!integrity->is_string() || !is_hex(integrity->as_string(), 64)) {
      return make_error(ErrorCode::SchemaViolation, "integrity must be a 64 character hex digest");
    }
    observation.declared_integrity = Digest256::from_hex(integrity->as_string());
  }
  FEL_TRY(observation.validate_shape());
  return observation;
}

Result<ObservationBatch> parse_evidence_document(std::string_view text, std::string_view origin_label) {
  ObservationBatch batch;
  batch.origin_label = std::string(origin_label);
  batch.document_digest = Sha256::hash(text);
  batch.format_version = kEvidenceFormatVersion;

  std::size_t offset = 0;
  bool saw_document = false;
  while (offset < text.size()) {
    std::size_t line_end = text.find('\n', offset);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }
    std::string_view line = text.substr(offset, line_end - offset);
    offset = line_end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
      ++start;
    }
    line.remove_prefix(start);
    if (line.empty()) {
      continue;
    }
    auto parsed = Json::parse(line);
    if (!parsed.has_value()) {
      return make_error(ErrorCode::MalformedJson, "evidence line is not valid JSON",
                        parsed.error().message);
    }
    Json document = parsed.value();
    if (document.is_object() && document.find("observations") != nullptr) {
      if (saw_document) {
        return make_error(ErrorCode::SchemaViolation,
                          "an evidence stream may contain only one document envelope");
      }
      saw_document = true;
      if (const Json* version = document.find("format_version"); version != nullptr) {
        std::uint64_t value = 0;
        FEL_TRY_ASSIGN(value, version->to_u64());
        batch.format_version = static_cast<std::uint32_t>(value);
      }
      FEL_TRY(document.reject_unknown_keys({"format_version", "observations", "origin_label"}));
      const Json* list = document.find("observations");
      if (list == nullptr || !list->is_array()) {
        return make_error(ErrorCode::SchemaViolation, "observations must be an array");
      }
      if (list->as_array().size() > limits::kMaxBatchRecords) {
        return make_error(ErrorCode::TooManyItems, "evidence document exceeds the record budget");
      }
      if (const Json* label = document.find("origin_label");
          label != nullptr && label->is_string()) {
        batch.origin_label = label->as_string();
      }
      for (const auto& item : list->as_array()) {
        FEL_TRY_ASSIGN(Observation observation, observation_from_json(item));
        batch.observations.push_back(std::move(observation));
      }
      continue;
    }
    FEL_TRY_ASSIGN(Observation observation, observation_from_json(document));
    batch.observations.push_back(std::move(observation));
  }
  if (batch.observations.size() > limits::kMaxBatchRecords) {
    return make_error(ErrorCode::TooManyItems, "evidence batch exceeds the record budget");
  }
  return batch;
}

Json period_to_json(const AccountingPeriod& period) {
  Json out = Json::object();
  out.set("id", Json::string(period.id.to_string()));
  out.set("ordinal", Json::number(period.ordinal.value()));
  out.set("label", Json::string(period.label));
  out.set("start", Json::string(period.start.to_rfc3339_millis()));
  out.set("end", Json::string(period.end.to_rfc3339_millis()));
  out.set("state", Json::string(std::string(period_state_name(period.state))));
  out.set("current_revision", Json::number(period.current_revision.value()));
  out.set("observations_ingested", Json::number(period.observations_ingested));
  return out;
}

Result<AccountingPeriod> period_from_json(const Json& document) {
  AccountingPeriod period;
  FEL_TRY_ASSIGN(const std::string id_text, document.require_string("id"));
  FEL_TRY_ASSIGN(period.id, PeriodId::parse(id_text));
  std::uint64_t number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("ordinal"));
  period.ordinal = Ordinal{number};
  FEL_TRY_ASSIGN(period.label, document.require_string("label"));
  FEL_TRY_ASSIGN(period.start, time_from_json(document, "start"));
  FEL_TRY_ASSIGN(period.end, time_from_json(document, "end"));
  FEL_TRY_ASSIGN(const std::string state_text, document.require_string("state"));
  if (state_text == "open") {
    period.state = PeriodState::Open;
  } else if (state_text == "closed") {
    period.state = PeriodState::Closed;
  } else if (state_text == "superseded") {
    period.state = PeriodState::Superseded;
  } else {
    return make_error(ErrorCode::InvalidArgument, "unknown period state", state_text);
  }
  number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("current_revision"));
  period.current_revision = RevisionOrdinal{number};
  FEL_TRY_ASSIGN(period.observations_ingested, document.require_u64("observations_ingested"));
  return period;
}

Json revision_to_json(const PeriodRevision& revision, bool include_evidence) {
  Json out = Json::object();
  out.set("schema", Json::string("fel.period-revision/v1"));
  out.set("period", Json::string(revision.period.to_string()));
  out.set("revision", Json::number(revision.revision.value()));
  out.set("parent_digest", Json::string(revision.parent_digest.has_value()
                                            ? revision.parent_digest->to_hex()
                                            : std::string("-")));
  out.set("correction", Json::string(revision.correction.has_value()
                                         ? revision.correction->to_string()
                                         : std::string("-")));
  out.set("correction_reason", Json::string(revision.correction_reason));
  out.set("closed_at", Json::string(revision.closed_at.to_rfc3339_millis()));
  out.set("policy_revision", Json::string(revision.policy_revision.to_string()));
  out.set("topology_revision", Json::string(revision.topology_revision.to_string()));
  out.set("store", Json::string(revision.store.to_string()));
  out.set("content_digest", Json::string(revision.content_digest.to_hex()));

  Json generations = Json::array();
  for (const auto& value : revision.generations) {
    generations.push(Json::string(value.to_string()));
  }
  out.set("generations", std::move(generations));

  Json claims = Json::array();
  for (const auto& claim : revision.claims) {
    Json item = Json::object();
    item.set("key", claim_key_to_json(claim.key));
    item.set("identity", Json::string(claim.identity.to_string()));
    item.set("known_total", Json::number(claim.cell.known_total));
    item.set("known_contributions", Json::number(claim.cell.known_contributions));
    item.set("unknown_contributions", Json::number(claim.cell.unknown_contributions));
    item.set("has_conflict", Json::boolean(claim.has_conflict));
    item.set("stale_included", Json::boolean(claim.stale_included));
    if (include_evidence) {
      Json evidence = Json::array();
      for (const auto& value : claim.evidence) {
        evidence.push(Json::string(value.to_string()));
      }
      item.set("evidence", std::move(evidence));
    }
    Json sources = Json::array();
    for (const auto& value : claim.sources) {
      sources.push(Json::string(value.to_string()));
    }
    item.set("sources", std::move(sources));
    Json provenance = Json::array();
    for (const auto value : claim.provenance) {
      provenance.push(Json::string(std::string(provenance_class_name(value))));
    }
    item.set("provenance", std::move(provenance));
    Json unknowns = Json::array();
    for (const auto& entry : claim.unknowns) {
      unknowns.push(unknown_to_json(entry));
    }
    item.set("unknowns", std::move(unknowns));
    claims.push(std::move(item));
  }
  out.set("claims", std::move(claims));

  Json unknowns = Json::array();
  for (const auto& entry : revision.unknowns) {
    unknowns.push(unknown_to_json(entry));
  }
  out.set("unknowns", std::move(unknowns));

  Json conflicts = Json::array();
  for (const auto& conflict : revision.conflicts) {
    Json item = Json::object();
    item.set("identity", Json::string(conflict.identity.to_string()));
    item.set("key", claim_key_to_json(conflict.key));
    item.set("distinct_amounts", Json::number(conflict.distinct_amounts));
    item.set("resolved", Json::boolean(conflict.resolved));
    item.set("resolution", Json::string(conflict.resolution));
    item.set("note", Json::string(conflict.note));
    Json participants = Json::array();
    for (const auto& value : conflict.participants) {
      participants.push(Json::string(value.to_string()));
    }
    item.set("participants", std::move(participants));
    Json sources = Json::array();
    for (const auto& value : conflict.sources) {
      sources.push(Json::string(value.to_string()));
    }
    item.set("sources", std::move(sources));
    conflicts.push(std::move(item));
  }
  out.set("conflicts", std::move(conflicts));

  Json rejected = Json::array();
  for (const auto& entry : revision.rejected) {
    Json item = Json::object();
    item.set("evidence", Json::string(entry.evidence.to_string()));
    item.set("source", Json::string(entry.source.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(entry.kind))));
    item.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
    item.set("detail", Json::string(entry.detail));
    rejected.push(std::move(item));
  }
  out.set("rejected", std::move(rejected));

  Json conservation = Json::array();
  for (const auto& domain : revision.conservation) {
    Json item = Json::object();
    item.set("scope", Json::string(domain.scope.is_nil() ? std::string("-")
                                                         : domain.scope.to_string()));
    item.set("generation", Json::string(domain.generation.to_string()));
    item.set("measure", Json::string(std::string(measure_kind_name(domain.kind))));
    item.set("attributed_total", Json::number(domain.attributed_total));
    item.set("unknown_contributions", Json::number(domain.unknown_contributions));
    item.set("contribution_count", Json::number(domain.contribution_count));
    item.set("observed_total", Json::string(domain.observed_total.has_value()
                                                ? std::to_string(*domain.observed_total)
                                                : std::string("-")));
    item.set("total_observations", Json::number(domain.total_observations));
    item.set("delta", Json::number(static_cast<std::uint64_t>(domain.delta)));
    item.set("delta_negative", Json::boolean(domain.delta < 0));
    item.set("residual_to_unknown", Json::number(domain.residual_to_unknown));
    item.set("status", Json::string(std::string(conservation_status_name(domain.status))));
    Json evidence = Json::array();
    for (const auto& value : domain.total_evidence) {
      evidence.push(Json::string(value.to_string()));
    }
    item.set("total_evidence", std::move(evidence));
    conservation.push(std::move(item));
  }
  out.set("conservation", std::move(conservation));

  Json efficiency = Json::array();
  for (const auto& summary : revision.efficiency) {
    Json item = Json::object();
    item.set("measure", Json::string(std::string(measure_kind_name(summary.kind))));
    item.set("coverage", Json::string(std::string(coverage_name(summary.coverage))));
    item.set("determinate", Json::boolean(summary.determinate));
    item.set("indeterminacy_reason", Json::string(summary.indeterminacy_reason));
    item.set("useful", Json::number(summary.useful));
    item.set("necessary_overhead", Json::number(summary.necessary_overhead));
    item.set("avoidable", Json::number(summary.avoidable));
    item.set("failed", Json::number(summary.failed));
    item.set("unknown_contributions", Json::number(summary.unknown_contributions));
    item.set("denominator", Json::number(summary.denominator));
    item.set("ratio_numerator", Json::number(summary.ratio_numerator));
    item.set("ratio_denominator", Json::number(summary.ratio_denominator));
    item.set("ratio_permille", Json::number(summary.ratio_permille));
    efficiency.push(std::move(item));
  }
  out.set("efficiency", std::move(efficiency));

  Json surfaces = Json::array();
  for (const auto value : revision.proof_surfaces) {
    surfaces.push(Json::string(std::string(provenance_class_name(value))));
  }
  out.set("proof_surfaces", std::move(surfaces));

  out.set("evidence_considered", Json::number(revision.evidence_considered));
  out.set("evidence_admitted", Json::number(revision.evidence_admitted));
  out.set("evidence_rejected", Json::number(revision.evidence_rejected));
  out.set("duplicate_suppressed", Json::number(revision.duplicate_suppressed));
  out.set("stale_included", Json::boolean(revision.stale_included));
  out.set("has_residual", Json::boolean(revision.has_residual));
  out.set("has_excess", Json::boolean(revision.has_excess));
  out.set("has_conflicts", Json::boolean(revision.has_conflicts));
  out.set("has_unknown", Json::boolean(revision.has_unknown));
  out.set("complete", Json::boolean(revision.complete));
  return out;
}

Result<PeriodRevision> revision_from_json(const Json& document) {
  PeriodRevision revision;
  FEL_TRY_ASSIGN(const std::string period_text, document.require_string("period"));
  FEL_TRY_ASSIGN(revision.period, PeriodId::parse(period_text));
  std::uint64_t number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("revision"));
  revision.revision = RevisionOrdinal{number};
  FEL_TRY_ASSIGN(const std::string parent_text, document.require_string("parent_digest"));
  if (parent_text != "-") {
    revision.parent_digest = Digest256::from_hex(parent_text);
  }
  FEL_TRY_ASSIGN(const std::string correction_text, document.require_string("correction"));
  if (correction_text != "-") {
    FEL_TRY_ASSIGN(revision.correction, CorrectionId::parse(correction_text));
  }
  FEL_TRY_ASSIGN(revision.correction_reason, document.require_string("correction_reason"));
  FEL_TRY_ASSIGN(revision.closed_at, time_from_json(document, "closed_at"));
  FEL_TRY_ASSIGN(const std::string policy_text, document.require_string("policy_revision"));
  FEL_TRY_ASSIGN(revision.policy_revision, PolicyRevisionId::parse(policy_text));
  FEL_TRY_ASSIGN(const std::string topology_text, document.require_string("topology_revision"));
  FEL_TRY_ASSIGN(revision.topology_revision, TopologyRevisionId::parse(topology_text));
  FEL_TRY_ASSIGN(const std::string store_text, document.require_string("store"));
  FEL_TRY_ASSIGN(revision.store, StoreId::parse(store_text));

  if (const Json* generations = document.find("generations");
      generations != nullptr && generations->is_array()) {
    for (const auto& item : generations->as_array()) {
      if (!item.is_string()) {
        return make_error(ErrorCode::SchemaViolation, "generation entry must be a string");
      }
      FEL_TRY_ASSIGN(const GenerationId id, GenerationId::parse(item.as_string()));
      revision.generations.push_back(id);
    }
  }

  if (const Json* claims = document.find("claims"); claims != nullptr && claims->is_array()) {
    for (const auto& item : claims->as_array()) {
      Claim claim;
      const Json* key = item.find("key");
      if (key == nullptr) {
        return make_error(ErrorCode::SchemaViolation, "claim is missing its key");
      }
      FEL_TRY_ASSIGN(claim.key, claim_key_from_json(*key));
      FEL_TRY_ASSIGN(const std::string identity_text, item.require_string("identity"));
      FEL_TRY_ASSIGN(claim.identity, AccountingIdentity::parse(identity_text));
      FEL_TRY_ASSIGN(claim.cell.known_total, item.require_u64("known_total"));
      FEL_TRY_ASSIGN(claim.cell.known_contributions, item.require_u64("known_contributions"));
      FEL_TRY_ASSIGN(claim.cell.unknown_contributions, item.require_u64("unknown_contributions"));
      claim.cell.kind = claim.key.kind;
      claim.period = revision.period;
      if (const Json* flag = item.find("has_conflict"); flag != nullptr && flag->is_bool()) {
        claim.has_conflict = flag->as_bool();
      }
      if (const Json* flag = item.find("stale_included"); flag != nullptr && flag->is_bool()) {
        claim.stale_included = flag->as_bool();
      }
      if (const Json* evidence = item.find("evidence");
          evidence != nullptr && evidence->is_array()) {
        for (const auto& value : evidence->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "evidence entry must be a string");
          }
          FEL_TRY_ASSIGN(const EvidenceId id, EvidenceId::parse(value.as_string()));
          claim.evidence.push_back(id);
        }
      }
      if (const Json* sources = item.find("sources"); sources != nullptr && sources->is_array()) {
        for (const auto& value : sources->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "source entry must be a string");
          }
          FEL_TRY_ASSIGN(const SourceId id, SourceId::parse(value.as_string()));
          claim.sources.push_back(id);
        }
      }
      if (const Json* provenance = item.find("provenance");
          provenance != nullptr && provenance->is_array()) {
        for (const auto& value : provenance->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "provenance entry must be a string");
          }
          const auto parsed = provenance_class_from_name(value.as_string());
          if (!parsed.has_value()) {
            return make_error(ErrorCode::InvalidArgument, "unknown provenance class",
                              value.as_string());
          }
          claim.provenance.push_back(parsed.value());
        }
      }
      if (const Json* unknowns = item.find("unknowns");
          unknowns != nullptr && unknowns->is_array()) {
        for (const auto& value : unknowns->as_array()) {
          FEL_TRY_ASSIGN(UnknownEntry entry, unknown_from_json(value));
          claim.unknowns.push_back(std::move(entry));
        }
      }
      claim.id = ClaimId::derive({revision.period.to_string(), claim.key.canonical_form(),
                                  claim.identity.to_string()});
      revision.claims.push_back(std::move(claim));
    }
  }

  if (const Json* unknowns = document.find("unknowns"); unknowns != nullptr && unknowns->is_array()) {
    for (const auto& item : unknowns->as_array()) {
      FEL_TRY_ASSIGN(UnknownEntry entry, unknown_from_json(item));
      revision.unknowns.push_back(std::move(entry));
    }
  }

  if (const Json* conflicts = document.find("conflicts");
      conflicts != nullptr && conflicts->is_array()) {
    for (const auto& item : conflicts->as_array()) {
      ConflictRecord record;
      FEL_TRY_ASSIGN(const std::string identity_text, item.require_string("identity"));
      FEL_TRY_ASSIGN(record.identity, AccountingIdentity::parse(identity_text));
      const Json* key = item.find("key");
      if (key != nullptr) {
        FEL_TRY_ASSIGN(record.key, claim_key_from_json(*key));
      }
      FEL_TRY_ASSIGN(record.distinct_amounts, item.require_u64("distinct_amounts"));
      if (const Json* flag = item.find("resolved"); flag != nullptr && flag->is_bool()) {
        record.resolved = flag->as_bool();
      }
      FEL_TRY_ASSIGN(record.resolution, item.require_string("resolution"));
      FEL_TRY_ASSIGN(record.note, item.require_string("note"));
      if (const Json* participants = item.find("participants");
          participants != nullptr && participants->is_array()) {
        for (const auto& value : participants->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "participant must be a string");
          }
          FEL_TRY_ASSIGN(const EvidenceId id, EvidenceId::parse(value.as_string()));
          record.participants.push_back(id);
        }
      }
      if (const Json* sources = item.find("sources"); sources != nullptr && sources->is_array()) {
        for (const auto& value : sources->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "source must be a string");
          }
          FEL_TRY_ASSIGN(const SourceId id, SourceId::parse(value.as_string()));
          record.sources.push_back(id);
        }
      }
      revision.conflicts.push_back(std::move(record));
    }
  }

  if (const Json* rejected = document.find("rejected"); rejected != nullptr && rejected->is_array()) {
    for (const auto& item : rejected->as_array()) {
      RejectedObservation entry;
      FEL_TRY_ASSIGN(const std::string evidence_text, item.require_string("evidence"));
      FEL_TRY_ASSIGN(entry.evidence, EvidenceId::parse(evidence_text));
      FEL_TRY_ASSIGN(const std::string source_text, item.require_string("source"));
      FEL_TRY_ASSIGN(entry.source, SourceId::parse(source_text));
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("measure"));
      const auto kind = measure_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind", kind_text);
      }
      entry.kind = kind.value();
      FEL_TRY_ASSIGN(const std::string reason_text, item.require_string("reason"));
      const auto reason = unknown_reason_from_name(reason_text);
      if (!reason.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown unknown-reason", reason_text);
      }
      entry.reason = reason.value();
      FEL_TRY_ASSIGN(entry.detail, item.require_string("detail"));
      revision.rejected.push_back(std::move(entry));
    }
  }

  if (const Json* conservation = document.find("conservation");
      conservation != nullptr && conservation->is_array()) {
    for (const auto& item : conservation->as_array()) {
      DomainConservation domain;
      FEL_TRY_ASSIGN(const std::string scope_text, item.require_string("scope"));
      if (scope_text != "-") {
        FEL_TRY_ASSIGN(domain.scope, ScopeId::parse(scope_text));
      }
      FEL_TRY_ASSIGN(const std::string generation_text, item.require_string("generation"));
      FEL_TRY_ASSIGN(domain.generation, GenerationId::parse(generation_text));
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("measure"));
      const auto kind = measure_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind", kind_text);
      }
      domain.kind = kind.value();
      FEL_TRY_ASSIGN(domain.attributed_total, item.require_u64("attributed_total"));
      FEL_TRY_ASSIGN(domain.unknown_contributions, item.require_u64("unknown_contributions"));
      FEL_TRY_ASSIGN(domain.contribution_count, item.require_u64("contribution_count"));
      FEL_TRY_ASSIGN(const std::string observed_text, item.require_string("observed_total"));
      if (observed_text != "-") {
        try {
          domain.observed_total = static_cast<std::uint64_t>(std::stoull(observed_text));
        } catch (const std::exception&) {
          return make_error(ErrorCode::SchemaViolation, "observed_total is not a number");
        }
      }
      FEL_TRY_ASSIGN(domain.total_observations, item.require_u64("total_observations"));
      std::uint64_t delta = 0;
      FEL_TRY_ASSIGN(delta, item.require_u64("delta"));
      bool negative = false;
      if (const Json* flag = item.find("delta_negative"); flag != nullptr && flag->is_bool()) {
        negative = flag->as_bool();
      }
      domain.delta = negative ? -static_cast<std::int64_t>(delta)
                              : static_cast<std::int64_t>(delta);
      FEL_TRY_ASSIGN(domain.residual_to_unknown, item.require_u64("residual_to_unknown"));
      FEL_TRY_ASSIGN(const std::string status_text, item.require_string("status"));
      if (status_text == "consistent") {
        domain.status = ConservationStatus::Consistent;
      } else if (status_text == "residual") {
        domain.status = ConservationStatus::Residual;
      } else if (status_text == "excess") {
        domain.status = ConservationStatus::Excess;
      } else if (status_text == "unverifiable") {
        domain.status = ConservationStatus::Unverifiable;
      } else {
        return make_error(ErrorCode::InvalidArgument, "unknown conservation status", status_text);
      }
      if (const Json* evidence = item.find("total_evidence");
          evidence != nullptr && evidence->is_array()) {
        for (const auto& value : evidence->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "evidence entry must be a string");
          }
          FEL_TRY_ASSIGN(const EvidenceId id, EvidenceId::parse(value.as_string()));
          domain.total_evidence.push_back(id);
        }
      }
      revision.conservation.push_back(std::move(domain));
    }
  }

  if (const Json* efficiency = document.find("efficiency");
      efficiency != nullptr && efficiency->is_array()) {
    for (const auto& item : efficiency->as_array()) {
      EfficiencySummary summary;
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("measure"));
      const auto kind = measure_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::UnknownMeasureKind, "unknown measure kind", kind_text);
      }
      summary.kind = kind.value();
      FEL_TRY_ASSIGN(const std::string coverage_text, item.require_string("coverage"));
      if (coverage_text == "known") {
        summary.coverage = Coverage::Known;
      } else if (coverage_text == "partial") {
        summary.coverage = Coverage::Partial;
      } else if (coverage_text == "unknown") {
        summary.coverage = Coverage::Unknown;
      } else {
        return make_error(ErrorCode::InvalidArgument, "unknown coverage value", coverage_text);
      }
      if (const Json* flag = item.find("determinate"); flag != nullptr && flag->is_bool()) {
        summary.determinate = flag->as_bool();
      }
      FEL_TRY_ASSIGN(summary.indeterminacy_reason, item.require_string("indeterminacy_reason"));
      FEL_TRY_ASSIGN(summary.useful, item.require_u64("useful"));
      FEL_TRY_ASSIGN(summary.necessary_overhead, item.require_u64("necessary_overhead"));
      FEL_TRY_ASSIGN(summary.avoidable, item.require_u64("avoidable"));
      FEL_TRY_ASSIGN(summary.failed, item.require_u64("failed"));
      FEL_TRY_ASSIGN(summary.unknown_contributions, item.require_u64("unknown_contributions"));
      FEL_TRY_ASSIGN(summary.denominator, item.require_u64("denominator"));
      FEL_TRY_ASSIGN(summary.ratio_numerator, item.require_u64("ratio_numerator"));
      FEL_TRY_ASSIGN(summary.ratio_denominator, item.require_u64("ratio_denominator"));
      FEL_TRY_ASSIGN(summary.ratio_permille, item.require_u64("ratio_permille"));
      revision.efficiency.push_back(std::move(summary));
    }
  }

  if (const Json* surfaces = document.find("proof_surfaces");
      surfaces != nullptr && surfaces->is_array()) {
    for (const auto& value : surfaces->as_array()) {
      if (!value.is_string()) {
        return make_error(ErrorCode::SchemaViolation, "proof surface must be a string");
      }
      const auto parsed = provenance_class_from_name(value.as_string());
      if (!parsed.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown provenance class",
                          value.as_string());
      }
      revision.proof_surfaces.push_back(parsed.value());
    }
  }

  FEL_TRY_ASSIGN(revision.evidence_considered, document.require_u64("evidence_considered"));
  FEL_TRY_ASSIGN(revision.evidence_admitted, document.require_u64("evidence_admitted"));
  FEL_TRY_ASSIGN(revision.evidence_rejected, document.require_u64("evidence_rejected"));
  FEL_TRY_ASSIGN(revision.duplicate_suppressed, document.require_u64("duplicate_suppressed"));
  const auto read_flag = [&](const char* key, bool* out) -> Result<void> {
    const Json* value = document.find(key);
    if (value != nullptr && value->is_bool()) {
      *out = value->as_bool();
    }
    return ok();
  };
  FEL_TRY(read_flag("stale_included", &revision.stale_included));
  FEL_TRY(read_flag("has_residual", &revision.has_residual));
  FEL_TRY(read_flag("has_excess", &revision.has_excess));
  FEL_TRY(read_flag("has_conflicts", &revision.has_conflicts));
  FEL_TRY(read_flag("has_unknown", &revision.has_unknown));
  FEL_TRY(read_flag("complete", &revision.complete));

  FEL_TRY_ASSIGN(const std::string digest_text, document.require_string("content_digest"));
  revision.content_digest = Digest256::from_hex(digest_text);
  FEL_TRY(revision.verify_digest());
  return revision;
}

Json correction_to_json(const CorrectionRecord& record) {
  Json out = Json::object();
  out.set("id", Json::string(record.id.to_string()));
  out.set("period", Json::string(record.period.to_string()));
  out.set("from_revision", Json::number(record.from_revision.value()));
  out.set("to_revision", Json::number(record.to_revision.value()));
  out.set("created_at", Json::string(record.created_at.to_rfc3339_millis()));
  out.set("reason", Json::string(record.reason));
  out.set("operator_label", Json::string(record.operator_label));
  out.set("parent_digest", Json::string(record.parent_digest.to_hex()));
  out.set("new_digest", Json::string(record.new_digest.to_hex()));
  out.set("previous_correction", Json::string(record.previous_correction.has_value()
                                                  ? record.previous_correction->to_hex()
                                                  : std::string("-")));
  Json evidence = Json::array();
  for (const auto& value : record.added_evidence) {
    evidence.push(Json::string(value.to_string()));
  }
  out.set("added_evidence", std::move(evidence));
  return out;
}

Result<CorrectionRecord> correction_from_json(const Json& document) {
  CorrectionRecord record;
  FEL_TRY_ASSIGN(const std::string id_text, document.require_string("id"));
  FEL_TRY_ASSIGN(record.id, CorrectionId::parse(id_text));
  FEL_TRY_ASSIGN(const std::string period_text, document.require_string("period"));
  FEL_TRY_ASSIGN(record.period, PeriodId::parse(period_text));
  std::uint64_t number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("from_revision"));
  record.from_revision = RevisionOrdinal{number};
  number = 0;
  FEL_TRY_ASSIGN(number, document.require_u64("to_revision"));
  record.to_revision = RevisionOrdinal{number};
  FEL_TRY_ASSIGN(record.created_at, time_from_json(document, "created_at"));
  FEL_TRY_ASSIGN(record.reason, document.require_string("reason"));
  FEL_TRY_ASSIGN(record.operator_label, document.require_string("operator_label"));
  FEL_TRY_ASSIGN(const std::string parent_text, document.require_string("parent_digest"));
  record.parent_digest = Digest256::from_hex(parent_text);
  FEL_TRY_ASSIGN(const std::string new_text, document.require_string("new_digest"));
  record.new_digest = Digest256::from_hex(new_text);
  FEL_TRY_ASSIGN(const std::string previous_text, document.require_string("previous_correction"));
  if (previous_text != "-") {
    if (!is_hex(previous_text, 64)) {
      return make_error(ErrorCode::SchemaViolation, "previous_correction is not a digest");
    }
    record.previous_correction = Digest256::from_hex(previous_text);
  }
  if (const Json* evidence = document.find("added_evidence");
      evidence != nullptr && evidence->is_array()) {
    for (const auto& value : evidence->as_array()) {
      if (!value.is_string()) {
        return make_error(ErrorCode::SchemaViolation, "evidence entry must be a string");
      }
      FEL_TRY_ASSIGN(const EvidenceId id, EvidenceId::parse(value.as_string()));
      record.added_evidence.push_back(id);
    }
  }
  return record;
}

Result<Json> parse_json_text(std::string_view text, std::string_view what) {
  auto parsed = Json::parse(text);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::MalformedJson, std::string(what) + " is not valid JSON",
                      parsed.error().message);
  }
  return parsed.value();
}

}  // namespace detail
}  // namespace fel
