// Fabric Efficiency Ledger - measures, categories, coverage, efficiency.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/measure.hpp"

#include <algorithm>
#include <numeric>
#include <string>

#include "fel/checked.hpp"

namespace fel {
namespace {

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> lookup_by_name(const char* const (&names)[N],
                                                 std::string_view name) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (name == names[i]) {
      return static_cast<Enum>(i + 1);
    }
  }
  return std::nullopt;
}

constexpr const char* kMeasureKindNames[] = {"wire-bytes",      "payload-bytes", "port-occupancy-nanos",
                                             "capacity-nanos",  "control-messages", "frame-count"};

constexpr const char* kMeasureKindUnits[] = {"octets", "octets", "nanoseconds",
                                             "nanoseconds", "messages", "frames"};

constexpr const char* kCategoryNames[] = {"useful-delivered-work", "retransmission",
                                          "duplication",           "reroute-overhead",
                                          "idle-reservation",      "failed-transfer",
                                          "stranded-capacity",     "control-overhead",
                                          "unknown-unattributed"};

constexpr const char* kUsefulnessNames[] = {"useful", "necessary-overhead", "avoidable", "failed",
                                            "unknown"};

constexpr const char* kUnknownReasonNames[] = {
    "none",
    "no-evidence",
    "stale-evidence",
    "expired-evidence",
    "fenced-epoch",
    "fenced-sequence",
    "retired-incarnation",
    "stale-generation",
    "conflicting-evidence",
    "duplicate-suppressed",
    "unsupported-provenance",
    "missing-measure-kind",
    "incomplete-window",
    "residual-unclassified",
    "attribution-failure",
    "clock-anomaly",
    "over-attribution",
    "format-unsupported"};

constexpr const char* kCoverageNames[] = {"known", "partial", "unknown"};

}  // namespace

std::string_view measure_kind_name(MeasureKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > kMeasureKindCount) {
    return "invalid";
  }
  return kMeasureKindNames[index - 1];
}

std::optional<MeasureKind> measure_kind_from_name(std::string_view name) noexcept {
  return lookup_by_name<MeasureKind>(kMeasureKindNames, name);
}

std::string_view measure_kind_unit(MeasureKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > kMeasureKindCount) {
    return "unknown";
  }
  return kMeasureKindUnits[index - 1];
}

std::string_view category_name(Category category) noexcept {
  const auto index = static_cast<std::size_t>(category);
  if (index == 0 || index > kCategoryCount) {
    return "invalid";
  }
  return kCategoryNames[index - 1];
}

std::optional<Category> category_from_name(std::string_view name) noexcept {
  return lookup_by_name<Category>(kCategoryNames, name);
}

Usefulness usefulness_of(Category category) noexcept {
  switch (category) {
    case Category::UsefulDeliveredWork:
      return Usefulness::Useful;
    case Category::ControlOverhead:
      return Usefulness::NecessaryOverhead;
    case Category::Retransmission:
    case Category::Duplication:
    case Category::RerouteOverhead:
    case Category::IdleReservation:
    case Category::StrandedCapacity:
      return Usefulness::Avoidable;
    case Category::FailedTransfer:
      return Usefulness::Failed;
    case Category::UnknownUnattributed:
      break;
  }
  return Usefulness::Unknown;
}

std::string_view usefulness_name(Usefulness usefulness) noexcept {
  const auto index = static_cast<std::size_t>(usefulness);
  if (index == 0 || index > 5) {
    return "invalid";
  }
  return kUsefulnessNames[index - 1];
}

std::string_view unknown_reason_name(UnknownReason reason) noexcept {
  const auto index = static_cast<std::size_t>(reason);
  if (index >= kUnknownReasonCount) {
    return "invalid";
  }
  return kUnknownReasonNames[index];
}

std::optional<UnknownReason> unknown_reason_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kUnknownReasonCount; ++i) {
    if (name == kUnknownReasonNames[i]) {
      return static_cast<UnknownReason>(i);
    }
  }
  return std::nullopt;
}

std::string_view coverage_name(Coverage coverage) noexcept {
  const auto index = static_cast<std::size_t>(coverage);
  if (index == 0 || index > 3) {
    return "invalid";
  }
  return kCoverageNames[index - 1];
}

Result<void> AggregateCell::add_known(std::uint64_t amount) {
  const auto next = add_checked(known_total, amount);
  if (!next.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow,
                      "aggregate cell total overflowed while accumulating " +
                          std::string(measure_kind_name(kind)));
  }
  known_total = *next;
  const auto count = add_checked(known_contributions, 1);
  if (!count.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "aggregate contribution count overflowed");
  }
  known_contributions = *count;
  return ok();
}

Result<void> AggregateCell::add_unknown() {
  const auto count = add_checked(unknown_contributions, 1);
  if (!count.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "aggregate unknown count overflowed");
  }
  unknown_contributions = *count;
  return ok();
}

Result<void> AggregateCell::merge(const AggregateCell& other) {
  if (other.kind != kind) {
    return make_error(ErrorCode::IncompatibleMeasureKind,
                      "refusing to merge aggregates of different measure kinds",
                      std::string(measure_kind_name(kind)) + " vs " +
                          std::string(measure_kind_name(other.kind)));
  }
  const auto total = add_checked(known_total, other.known_total);
  if (!total.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "aggregate total overflowed while merging");
  }
  const auto known = add_checked(known_contributions, other.known_contributions);
  if (!known.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow,
                      "aggregate known count overflowed while merging");
  }
  const auto unknown = add_checked(unknown_contributions, other.unknown_contributions);
  if (!unknown.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow,
                      "aggregate unknown count overflowed while merging");
  }
  known_total = *total;
  known_contributions = *known;
  unknown_contributions = *unknown;
  return ok();
}

std::string EfficiencySummary::ratio_text() const {
  if (!determinate) {
    return "indeterminate (" + indeterminacy_reason + ")";
  }
  if (ratio_denominator == 0) {
    return "0/0";
  }
  return std::to_string(ratio_numerator) + "/" + std::to_string(ratio_denominator);
}

}  // namespace fel
