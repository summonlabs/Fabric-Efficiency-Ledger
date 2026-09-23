// Fabric Efficiency Ledger - evidence records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/crypto.hpp"
#include "fel/id.hpp"
#include "fel/measure.hpp"
#include "fel/time.hpp"

namespace fel {

// A metadata pair. Bounded in count, key length and value length. Metadata never
// participates in accounting; it is carried for audit and explanation.
struct MetadataEntry {
  std::string key;
  std::string value;

  friend bool operator==(const MetadataEntry& a, const MetadataEntry& b) noexcept {
    return a.key == b.key && a.value == b.value;
  }
  friend bool operator<(const MetadataEntry& a, const MetadataEntry& b) noexcept {
    if (a.key != b.key) return a.key < b.key;
    return a.value < b.value;
  }
};

// A contribution adds to a category. A total states what the source believes the
// whole domain consumed; it is the independent denominator used by conservation
// checks and never contributes to a category itself.
enum class ObservationRole : std::uint8_t {
  Contribution = 1,
  Total = 2,
};

[[nodiscard]] std::string_view observation_role_name(ObservationRole role) noexcept;
[[nodiscard]] std::optional<ObservationRole> observation_role_from_name(std::string_view name) noexcept;

// One immutable unit of evidence.
//
// Every observation states what was observed, by which source and source
// incarnation, for which fabric generation, at which observation and receive
// times, and under which source epoch and sequence. Nothing is implicit.
struct Observation {
  EvidenceId id;
  // Delivery identity shared by every copy of the same physical evidence. A
  // mirrored re-delivery keeps the origin and therefore cannot be counted twice.
  OriginId origin;
  SourceId source;
  IncarnationId incarnation;
  EpochId epoch;
  SourceSequence sequence;
  GenerationId generation;

  TimePoint observed_at;
  TimePoint received_at;

  MeasureKind kind = MeasureKind::WireBytes;
  ObservationRole role = ObservationRole::Contribution;
  // Meaningful only when role == Contribution. A Total observation must use
  // Category::UnknownUnattributed as a placeholder.
  Category category = Category::UnknownUnattributed;

  std::optional<ResourceId> resource;
  std::optional<FlowId> flow;
  std::optional<PathId> path;
  std::optional<ReservationId> reservation;

  std::uint64_t amount = 0;

  // The reporting window the measurement covers. Conservation only compares
  // observations whose windows are compatible with the accounting period.
  TimePoint window_start;
  TimePoint window_end;

  std::vector<MetadataEntry> metadata;

  // Integrity of the record as declared by the producer. Verified when present.
  std::optional<Digest256> declared_integrity;

  [[nodiscard]] std::string canonical_form() const;
  // Digest of every field except received_at, integrity and id. Two deliveries
  // of the same evidence through different mirrors agree on this digest.
  [[nodiscard]] Digest256 content_digest() const;
  [[nodiscard]] Digest256 record_digest() const;
  [[nodiscard]] Result<void> validate_shape() const;
  [[nodiscard]] const MetadataEntry* find_metadata(std::string_view key) const noexcept;
  [[nodiscard]] std::string_view attribution_key() const;
};

// A bounded batch of observations plus the digest of the document they came from.
struct ObservationBatch {
  std::vector<Observation> observations;
  Digest256 document_digest;
  std::string origin_label;
  std::uint32_t format_version = 0;
};

// Counters describing what ingest did. Deterministic given the same input and
// policy: the same batch always yields the same report.
struct IngestReport {
  std::uint64_t received = 0;
  std::uint64_t accepted = 0;
  std::uint64_t duplicate_suppressed = 0;
  std::uint64_t rejected_shape = 0;
  std::uint64_t rejected_unknown_source = 0;
  std::uint64_t rejected_retired_incarnation = 0;
  std::uint64_t rejected_epoch_fenced = 0;
  std::uint64_t rejected_sequence_fenced = 0;
  std::uint64_t rejected_generation = 0;
  std::uint64_t rejected_unsupported_provenance = 0;
  std::uint64_t rejected_capacity = 0;
  std::uint64_t rejected_no_period = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t late_evidence = 0;
  Digest256 batch_digest;
  std::vector<std::string> diagnostics;

  [[nodiscard]] std::string canonical_form() const;
};

}  // namespace fel
