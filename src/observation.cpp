// Fabric Efficiency Ledger - evidence records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/observation.hpp"

#include <algorithm>
#include <string>

#include "fel/limits.hpp"
#include "fel/serialize.hpp"

namespace fel {

std::string_view observation_role_name(ObservationRole role) noexcept {
  switch (role) {
    case ObservationRole::Contribution:
      return "contribution";
    case ObservationRole::Total:
      return "total";
  }
  return "invalid";
}

std::optional<ObservationRole> observation_role_from_name(std::string_view name) noexcept {
  if (name == "contribution") {
    return ObservationRole::Contribution;
  }
  if (name == "total") {
    return ObservationRole::Total;
  }
  return std::nullopt;
}

Result<void> Observation::validate_shape() const {
  if (window_end < window_start) {
    return make_error(ErrorCode::InvalidArgument, "observation window ends before it starts",
                      id.to_string());
  }
  if (window_end == window_start) {
    return make_error(ErrorCode::InvalidArgument, "observation window has zero width",
                      id.to_string());
  }
  if (observed_at.is_zero() || received_at.is_zero()) {
    return make_error(ErrorCode::InvalidArgument,
                      "observation must carry an observation time and a receive time",
                      id.to_string());
  }
  if (window_start.is_zero() || window_end.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "observation must declare its reporting window",
                      id.to_string());
  }
  if (metadata.size() > limits::kMaxMetadataEntries) {
    return make_error(ErrorCode::MetadataTooLarge, "observation metadata exceeds the entry budget",
                      id.to_string());
  }
  for (const auto& entry : metadata) {
    if (entry.key.empty()) {
      return make_error(ErrorCode::SchemaViolation, "metadata key must not be empty");
    }
    if (entry.key.size() > limits::kMaxMetadataKeyBytes) {
      return make_error(ErrorCode::MetadataTooLarge, "metadata key exceeds the key budget",
                        entry.key);
    }
    if (entry.value.size() > limits::kMaxMetadataValueBytes) {
      return make_error(ErrorCode::MetadataTooLarge, "metadata value exceeds the value budget",
                        entry.key);
    }
  }
  std::vector<MetadataEntry> sorted = metadata;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return make_error(ErrorCode::SchemaViolation, "observation metadata repeats a key",
                      id.to_string());
  }
  if (role == ObservationRole::Total && category != Category::UnknownUnattributed) {
    return make_error(ErrorCode::SchemaViolation,
                      "a total observation must not declare a classification category",
                      id.to_string());
  }
  if (role == ObservationRole::Contribution && category == Category::UnknownUnattributed) {
    // Permitted: the producer measured a quantity but could not classify it.
    // The quantity stays in the explicit unknown bucket.
  }
  return ok();
}

const MetadataEntry* Observation::find_metadata(std::string_view key) const noexcept {
  for (const auto& entry : metadata) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

std::string_view Observation::attribution_key() const {
  if (resource.has_value()) {
    return "resource";
  }
  if (path.has_value()) {
    return "path";
  }
  if (flow.has_value()) {
    return "flow";
  }
  if (reservation.has_value()) {
    return "reservation";
  }
  return "unattributed";
}

std::string Observation::canonical_form() const {
  FieldWriter writer;
  writer.field("observation/1");
  writer.field(id.to_string());
  writer.field(origin.to_string());
  writer.field(source.to_string());
  writer.field(incarnation.to_string());
  writer.field_u64(epoch.value());
  writer.field_u64(sequence.value());
  writer.field(generation.to_string());
  writer.field_i64(observed_at.nanos());
  writer.field_i64(received_at.nanos());
  writer.field(measure_kind_name(kind));
  writer.field(observation_role_name(role));
  writer.field(category_name(category));
  writer.field(resource.has_value() ? resource->to_string() : std::string("-"));
  writer.field(flow.has_value() ? flow->to_string() : std::string("-"));
  writer.field(path.has_value() ? path->to_string() : std::string("-"));
  writer.field(reservation.has_value() ? reservation->to_string() : std::string("-"));
  writer.field_u64(amount);
  writer.field_i64(window_start.nanos());
  writer.field_i64(window_end.nanos());
  std::vector<MetadataEntry> sorted = metadata;
  std::sort(sorted.begin(), sorted.end());
  for (const auto& entry : sorted) {
    writer.field(entry.key);
    writer.field(entry.value);
  }
  return writer.text();
}

Digest256 Observation::content_digest() const {
  FieldWriter writer;
  writer.field("observation-content/1");
  writer.field(origin.to_string());
  writer.field(source.to_string());
  writer.field(incarnation.to_string());
  writer.field_u64(epoch.value());
  writer.field_u64(sequence.value());
  writer.field(generation.to_string());
  writer.field_i64(observed_at.nanos());
  writer.field(measure_kind_name(kind));
  writer.field(observation_role_name(role));
  writer.field(category_name(category));
  writer.field(resource.has_value() ? resource->to_string() : std::string("-"));
  writer.field(flow.has_value() ? flow->to_string() : std::string("-"));
  writer.field(path.has_value() ? path->to_string() : std::string("-"));
  writer.field(reservation.has_value() ? reservation->to_string() : std::string("-"));
  writer.field_u64(amount);
  writer.field_i64(window_start.nanos());
  writer.field_i64(window_end.nanos());
  std::vector<MetadataEntry> sorted = metadata;
  std::sort(sorted.begin(), sorted.end());
  for (const auto& entry : sorted) {
    writer.field(entry.key);
    writer.field(entry.value);
  }
  return writer.digest();
}

Digest256 Observation::record_digest() const {
  FieldWriter writer;
  writer.field(canonical_form());
  writer.field_i64(received_at.nanos());
  return writer.digest();
}

std::string IngestReport::canonical_form() const {
  FieldWriter writer;
  writer.field("ingest-report/1");
  writer.field_u64(received);
  writer.field_u64(accepted);
  writer.field_u64(duplicate_suppressed);
  writer.field_u64(rejected_shape);
  writer.field_u64(rejected_unknown_source);
  writer.field_u64(rejected_retired_incarnation);
  writer.field_u64(rejected_epoch_fenced);
  writer.field_u64(rejected_sequence_fenced);
  writer.field_u64(rejected_generation);
  writer.field_u64(rejected_unsupported_provenance);
  writer.field_u64(rejected_capacity);
  writer.field_u64(rejected_no_period);
  writer.field_u64(conflicts);
  writer.field_u64(late_evidence);
  writer.field(batch_digest.to_hex());
  for (const auto& line : diagnostics) {
    writer.field(line);
  }
  return writer.text();
}

}  // namespace fel
