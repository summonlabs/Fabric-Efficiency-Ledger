// Fabric Efficiency Ledger - measures, categories, and coverage semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/id.hpp"

namespace fel {

// ---------------------------------------------------------------------------
// Measure kinds
//
// A quantity is only meaningful together with its unit. Conservation checks and
// aggregations are performed strictly per measure kind: octets are never summed
// with nanoseconds, and a conservation equation that would mix them is refused
// with ErrorCode::IncompatibleMeasureKind.
// ---------------------------------------------------------------------------
enum class MeasureKind : std::uint8_t {
  WireBytes = 1,          // octets offered to the fabric, including headers
  PayloadBytes = 2,       // octets of application payload
  PortOccupancyNanos = 3, // resource-time a port or queue was busy
  CapacityNanos = 4,      // reserved or stranded capacity-time
  ControlMessages = 5,    // discrete control / discovery messages
  FrameCount = 6,         // discrete frames or packets
};

inline constexpr std::size_t kMeasureKindCount = 6;

[[nodiscard]] std::string_view measure_kind_name(MeasureKind kind) noexcept;
[[nodiscard]] std::optional<MeasureKind> measure_kind_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view measure_kind_unit(MeasureKind kind) noexcept;

// ---------------------------------------------------------------------------
// Accounting categories
//
// The category set is closed. Every contributed quantity lands in exactly one
// category, which is what makes double counting detectable: the sum of category
// totals must equal the independently reported total for the same
// (scope, generation, measure kind) domain.
// ---------------------------------------------------------------------------
enum class Category : std::uint8_t {
  UsefulDeliveredWork = 1,
  Retransmission = 2,
  Duplication = 3,
  RerouteOverhead = 4,
  IdleReservation = 5,
  FailedTransfer = 6,
  StrandedCapacity = 7,
  ControlOverhead = 8,
  UnknownUnattributed = 9,
};

inline constexpr std::size_t kCategoryCount = 9;

// The classification of a category in efficiency terms.
enum class Usefulness : std::uint8_t {
  Useful = 1,             // work that the consumer actually received
  NecessaryOverhead = 2,  // measurable protocol overhead that cannot be avoided
  Avoidable = 3,          // consumption that a different decision would have removed
  Failed = 4,             // consumption that produced no delivered work
  Unknown = 5,            // not classified; never counted as useful
};

[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Category> category_from_name(std::string_view name) noexcept;
[[nodiscard]] Usefulness usefulness_of(Category category) noexcept;
[[nodiscard]] std::string_view usefulness_name(Usefulness usefulness) noexcept;

// Why a quantity could not be attributed to a known bucket. This is a closed
// set: "unknown" is always explained, never merely asserted.
enum class UnknownReason : std::uint8_t {
  None = 0,
  NoEvidence = 1,              // the source produced nothing for this domain
  StaleEvidence = 2,           // evidence older than the freshness budget
  ExpiredEvidence = 3,         // evidence older than the retention budget
  FencedEpoch = 4,             // superseded source epoch
  FencedSequence = 5,          // replayed or regressed source sequence
  RetiredIncarnation = 6,      // produced by a source incarnation that has ended
  StaleGeneration = 7,         // belongs to a superseded fabric generation
  ConflictingEvidence = 8,     // two authoritative sources disagree
  DuplicateSuppressed = 9,     // a duplicate delivery was counted once
  UnsupportedProvenance = 10,  // the source is declared unsupported
  MissingMeasureKind = 11,     // the source did not report this unit
  IncompleteWindow = 12,       // the reporting window does not cover the period
  ResidualUnclassified = 13,   // the observed total exceeds attributed categories
  AttributionFailure = 14,     // no attribution rule matched the evidence
  ClockAnomaly = 15,           // observation time after receive time, or in the future
  OverAttribution = 16,        // attributed total exceeded the observed total
  FormatUnsupported = 17,      // evidence carried an unsupported schema version
};

inline constexpr std::size_t kUnknownReasonCount = 18;

[[nodiscard]] std::string_view unknown_reason_name(UnknownReason reason) noexcept;
[[nodiscard]] std::optional<UnknownReason> unknown_reason_from_name(std::string_view name) noexcept;

// How much of a domain is actually known.
enum class Coverage : std::uint8_t {
  Known = 1,    // every contribution in the domain carried a usable measurement
  Partial = 2,  // some contributions are known, some are not
  Unknown = 3,  // nothing usable was observed; the value is NOT zero
};

[[nodiscard]] std::string_view coverage_name(Coverage coverage) noexcept;

// ---------------------------------------------------------------------------
// Quantities
// ---------------------------------------------------------------------------

// A single measured quantity with an explicit unit.
struct Quantity {
  MeasureKind kind = MeasureKind::WireBytes;
  std::uint64_t value = 0;

  friend bool operator==(const Quantity& a, const Quantity& b) noexcept {
    return a.kind == b.kind && a.value == b.value;
  }
  friend bool operator<(const Quantity& a, const Quantity& b) noexcept {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.value < b.value;
  }
};

// An aggregated cell. It carries the known total, how many contributions were
// folded in, and how many contributions existed but could not be measured. A
// cell with unknown_contributions > 0 and known_contributions == 0 has an
// unknown value; reporting zero there would be inventing data.
struct AggregateCell {
  MeasureKind kind = MeasureKind::WireBytes;
  std::uint64_t known_total = 0;
  std::uint64_t known_contributions = 0;
  std::uint64_t unknown_contributions = 0;

  [[nodiscard]] Coverage coverage() const noexcept {
    if (unknown_contributions == 0) {
      return Coverage::Known;
    }
    if (known_contributions == 0) {
      return Coverage::Unknown;
    }
    return Coverage::Partial;
  }

  [[nodiscard]] bool has_value() const noexcept { return known_contributions > 0; }
  [[nodiscard]] std::optional<std::uint64_t> value_if_known() const noexcept {
    if (coverage() == Coverage::Known) {
      return known_total;
    }
    return std::nullopt;
  }

  // There is deliberately no unchecked "add": a cell that silently wraps would
  // make conservation meaningless. Every accumulation is checked.
  [[nodiscard]] Result<void> add_known(std::uint64_t amount);
  [[nodiscard]] Result<void> add_unknown();
  [[nodiscard]] Result<void> merge(const AggregateCell& other);
};

// One entry of the explicit unknown bucket. Unknown entries never carry a
// numeric amount: there is nothing to carry.
struct UnknownEntry {
  ScopeId scope;
  MeasureKind kind = MeasureKind::WireBytes;
  UnknownReason reason = UnknownReason::NoEvidence;
  std::uint64_t evidence_count = 0;
  GenerationId generation;
  std::string note;

  friend bool operator==(const UnknownEntry& a, const UnknownEntry& b) noexcept {
    return a.scope == b.scope && a.kind == b.kind && a.reason == b.reason &&
           a.evidence_count == b.evidence_count && a.generation == b.generation &&
           a.note == b.note;
  }
  friend bool operator!=(const UnknownEntry& a, const UnknownEntry& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const UnknownEntry& a, const UnknownEntry& b) noexcept {
    if (a.scope != b.scope) return a.scope < b.scope;
    if (a.generation != b.generation) return a.generation < b.generation;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.reason != b.reason) return a.reason < b.reason;
    return a.note < b.note;
  }
};

// ---------------------------------------------------------------------------
// Efficiency summary
//
// The ratio is computed with exact integer arithmetic only. When any part of the
// domain is unknown, the ratio is reported as indeterminate and the reason is
// stated: the ledger never derives an efficiency number from missing telemetry.
// ---------------------------------------------------------------------------
struct EfficiencySummary {
  MeasureKind kind = MeasureKind::WireBytes;
  Coverage coverage = Coverage::Unknown;
  bool determinate = false;
  std::string indeterminacy_reason;

  std::uint64_t useful = 0;
  std::uint64_t necessary_overhead = 0;
  std::uint64_t avoidable = 0;
  std::uint64_t failed = 0;
  std::uint64_t unknown_contributions = 0;

  std::uint64_t denominator = 0;  // useful + necessary + avoidable + failed
  // Exact ratio useful/denominator, reduced. Stored as integers so that two
  // runs on two machines produce byte identical output.
  std::uint64_t ratio_numerator = 0;
  std::uint64_t ratio_denominator = 0;
  std::uint64_t ratio_permille = 0;  // rounded half up, 0..1000

  [[nodiscard]] std::string ratio_text() const;
};

}  // namespace fel
