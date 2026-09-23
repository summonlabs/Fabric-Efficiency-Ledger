// Fabric Efficiency Ledger - bounded hierarchical and time aggregation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/aggregate.hpp"

#include <string>

#include "fel/serialize.hpp"

namespace fel {
namespace {

constexpr const char* kAxisNames[] = {"category", "scope",     "generation", "source",
                                      "time",     "provenance", "resource",   "flow"};

[[nodiscard]] std::string optional_identity(const StrongId<ScopeTag>& id) { return id.to_string(); }

}  // namespace

std::string_view aggregate_axis_name(AggregateAxis axis) noexcept {
  const auto index = static_cast<std::size_t>(axis);
  if (index == 0 || index > kAggregateAxisCount) {
    return "invalid";
  }
  return kAxisNames[index - 1];
}

std::optional<AggregateAxis> aggregate_axis_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kAggregateAxisCount; ++i) {
    if (name == kAxisNames[i]) {
      return static_cast<AggregateAxis>(i + 1);
    }
  }
  return std::nullopt;
}

std::string AggregateKey::label() const {
  switch (axis) {
    case AggregateAxis::Category:
      return std::string(category_name(category));
    case AggregateAxis::Scope:
      return scope.to_string();
    case AggregateAxis::Generation:
      return generation.to_string();
    case AggregateAxis::Source:
      return source.to_string();
    case AggregateAxis::Time:
      return bucket_start.to_rfc3339();
    case AggregateAxis::Provenance:
      return std::string(provenance_class_name(provenance));
    case AggregateAxis::Resource:
      return resource.to_string();
    case AggregateAxis::Flow:
      return flow.to_string();
  }
  return "invalid";
}

std::string AggregateKey::canonical_form() const {
  FieldWriter writer;
  writer.field("aggregate-key/1");
  writer.field(aggregate_axis_name(axis));
  writer.field(category_name(category));
  writer.field(scope.to_string());
  writer.field(generation.to_string());
  writer.field(source.to_string());
  writer.field(resource.to_string());
  writer.field(flow.to_string());
  writer.field(provenance_class_name(provenance));
  writer.field_i64(bucket_start.nanos());
  writer.field_i64(bucket_end.nanos());
  return writer.text();
}

bool operator==(const AggregateKey& a, const AggregateKey& b) noexcept {
  return a.axis == b.axis && a.category == b.category && a.scope == b.scope &&
         a.generation == b.generation && a.source == b.source && a.resource == b.resource &&
         a.flow == b.flow && a.provenance == b.provenance && a.bucket_start == b.bucket_start &&
         a.bucket_end == b.bucket_end;
}

bool operator<(const AggregateKey& a, const AggregateKey& b) noexcept {
  if (a.axis != b.axis) return a.axis < b.axis;
  if (a.category != b.category) return a.category < b.category;
  if (a.scope != b.scope) return a.scope < b.scope;
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.source != b.source) return a.source < b.source;
  if (a.resource != b.resource) return a.resource < b.resource;
  if (a.flow != b.flow) return a.flow < b.flow;
  if (a.provenance != b.provenance) return a.provenance < b.provenance;
  if (a.bucket_start != b.bucket_start) return a.bucket_start < b.bucket_start;
  return a.bucket_end < b.bucket_end;
}

bool AggregateBucket::has_unknown() const noexcept {
  for (const auto& cell : cells) {
    if (cell.unknown_contributions > 0) {
      return true;
    }
  }
  return !unknowns.empty();
}

std::string AggregateBucket::canonical_form() const {
  FieldWriter writer;
  writer.field("aggregate-bucket/1");
  writer.field(key.canonical_form());
  writer.field_u64(claim_count);
  writer.field_u64(evidence_count);
  for (const auto& cell : cells) {
    writer.field(measure_kind_name(cell.kind));
    writer.field_u64(cell.known_total);
    writer.field_u64(cell.known_contributions);
    writer.field_u64(cell.unknown_contributions);
  }
  for (const auto& entry : unknowns) {
    writer.field(entry.scope.to_string());
    writer.field(entry.generation.to_string());
    writer.field(measure_kind_name(entry.kind));
    writer.field(unknown_reason_name(entry.reason));
    writer.field_u64(entry.evidence_count);
    writer.field(entry.note);
  }
  for (const auto value : proof_surfaces) {
    writer.field(provenance_class_name(value));
  }
  return writer.text();
}

std::string AggregationResult::canonical_form() const {
  FieldWriter writer;
  writer.field("aggregation-result/1");
  writer.field(aggregate_axis_name(axis));
  writer.field(window.has_value() ? std::to_string(window->nanos()) : std::string("-"));
  writer.field_i64(window_start.nanos());
  writer.field_i64(window_end.nanos());
  writer.field_u64(truncation.total_available);
  writer.field_u64(truncation.returned);
  writer.field_bool(truncation.truncated);
  writer.field(truncation.reason);
  writer.field(stamp.canonical_form());
  writer.field_u64(static_cast<std::uint64_t>(buckets.size()));
  for (const auto& bucket : buckets) {
    writer.field(bucket.canonical_form());
  }
  return writer.text();
}

std::string AggregationResult::to_json() const {
  Json root = Json::object();
  root.set("schema", Json::string("fel.aggregation/v1"));
  root.set("axis", Json::string(std::string(aggregate_axis_name(axis))));
  if (window.has_value()) {
    root.set("window_nanos", Json::number(static_cast<std::uint64_t>(window->nanos())));
  }
  root.set("window_start", Json::string(window_start.to_rfc3339()));
  root.set("window_end", Json::string(window_end.to_rfc3339()));
  root.set("truncated", Json::boolean(truncation.truncated));
  root.set("total_available", Json::number(truncation.total_available));
  root.set("returned", Json::number(truncation.returned));
  if (!truncation.reason.empty()) {
    root.set("truncation_reason", Json::string(truncation.reason));
  }
  root.set("digest", Json::string(digest.to_hex()));
  root.set("proof_surfaces", Json::string(stamp.proof_surface_label()));

  Json provenance = Json::object();
  provenance.set("policy_revision", Json::string(stamp.policy_revision.to_string()));
  provenance.set("topology_revision", Json::string(stamp.topology_revision.to_string()));
  provenance.set("store", Json::string(stamp.store.to_string()));
  provenance.set("store_revision", Json::number(stamp.store_revision));
  provenance.set("as_of", Json::string(stamp.as_of.to_rfc3339()));
  provenance.set("store_watermark", Json::string(stamp.store_watermark.to_rfc3339()));
  provenance.set("freshness_rebased", Json::boolean(stamp.freshness_rebased));
  provenance.set("contains_stale", Json::boolean(stamp.contains_stale));
  provenance.set("contains_unknown", Json::boolean(stamp.contains_unknown));
  Json generations = Json::array();
  for (const auto& value : stamp.generations) {
    generations.push(Json::string(value.to_string()));
  }
  provenance.set("generations", std::move(generations));
  root.set("provenance", std::move(provenance));

  Json bucket_array = Json::array();
  for (const auto& bucket : buckets) {
    Json item = Json::object();
    item.set("label", Json::string(bucket.key.label()));
    item.set("key", Json::string(bucket.key.canonical_form()));
    item.set("claim_count", Json::number(bucket.claim_count));
    item.set("evidence_count", Json::number(bucket.evidence_count));
    Json cells = Json::array();
    for (const auto& cell : bucket.cells) {
      Json entry = Json::object();
      entry.set("measure", Json::string(std::string(measure_kind_name(cell.kind))));
      entry.set("unit", Json::string(std::string(measure_kind_unit(cell.kind))));
      entry.set("coverage", Json::string(std::string(coverage_name(cell.coverage()))));
      entry.set("known_total", Json::number(cell.known_total));
      entry.set("known_contributions", Json::number(cell.known_contributions));
      entry.set("unknown_contributions", Json::number(cell.unknown_contributions));
      cells.push(std::move(entry));
    }
    item.set("cells", std::move(cells));
    Json unknowns = Json::array();
    for (const auto& entry : bucket.unknowns) {
      Json unknown = Json::object();
      unknown.set("scope", Json::string(entry.scope.to_string()));
      unknown.set("generation", Json::string(entry.generation.to_string()));
      unknown.set("measure", Json::string(std::string(measure_kind_name(entry.kind))));
      unknown.set("reason", Json::string(std::string(unknown_reason_name(entry.reason))));
      unknown.set("evidence_count", Json::number(entry.evidence_count));
      if (!entry.note.empty()) {
        unknown.set("note", Json::string(entry.note));
      }
      unknowns.push(std::move(unknown));
    }
    item.set("unknowns", std::move(unknowns));
    bucket_array.push(std::move(item));
  }
  root.set("bucket_array", std::move(bucket_array));
  return root.dump(2);
}

}  // namespace fel