// Fabric Efficiency Ledger - bounded hierarchical and time aggregation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/checked.hpp"
#include "fel/id.hpp"
#include "fel/measure.hpp"
#include "fel/provenance.hpp"
#include "fel/time.hpp"

namespace fel {

enum class AggregateAxis : std::uint8_t {
  Category = 1,
  Scope = 2,
  Generation = 3,
  Source = 4,
  Time = 5,
  Provenance = 6,
  Resource = 7,
  Flow = 8,
};

inline constexpr std::size_t kAggregateAxisCount = 8;

[[nodiscard]] std::string_view aggregate_axis_name(AggregateAxis axis) noexcept;
[[nodiscard]] std::optional<AggregateAxis> aggregate_axis_from_name(std::string_view name) noexcept;

// A bucket key. Exactly one axis is active per aggregation; the remaining fields
// are nil. Ordering is total, so bucket order in the output is deterministic.
struct AggregateKey {
  AggregateAxis axis = AggregateAxis::Category;
  Category category = Category::UnknownUnattributed;
  ScopeId scope;
  GenerationId generation;
  SourceId source;
  ResourceId resource;
  FlowId flow;
  ProvenanceClass provenance = ProvenanceClass::Real;
  TimePoint bucket_start;
  TimePoint bucket_end;

  [[nodiscard]] std::string label() const;
  [[nodiscard]] std::string canonical_form() const;

  friend bool operator<(const AggregateKey& a, const AggregateKey& b) noexcept;
  friend bool operator==(const AggregateKey& a, const AggregateKey& b) noexcept;
};

struct AggregateBucket {
  AggregateKey key;
  std::vector<AggregateCell> cells;      // ascending by measure kind
  std::vector<UnknownEntry> unknowns;    // ascending, deduplicated
  std::uint64_t claim_count = 0;
  std::uint64_t evidence_count = 0;
  std::vector<ProvenanceClass> proof_surfaces;

  [[nodiscard]] bool has_unknown() const noexcept;
  [[nodiscard]] std::string canonical_form() const;
};

struct AggregationResult {
  AggregateAxis axis = AggregateAxis::Category;
  std::optional<Duration> window;
  TimePoint window_start;
  TimePoint window_end;
  std::vector<AggregateBucket> buckets;
  Truncation truncation;
  ProvenanceStamp stamp;
  Digest256 digest;

  [[nodiscard]] std::string canonical_form() const;
  [[nodiscard]] std::string to_json() const;
};

}  // namespace fel
