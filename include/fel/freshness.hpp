// Fabric Efficiency Ledger - evidence freshness assessment.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string>
#include <string_view>

#include "fel/measure.hpp"
#include "fel/observation.hpp"
#include "fel/policy.hpp"
#include "fel/topology.hpp"

namespace fel {

// Freshness is always relative to an explicit "as of" instant. The ledger never
// consults the wall clock on its own: a restarted process must not be able to
// silently promote persisted evidence back to fresh.
enum class FreshnessClass : std::uint8_t {
  Fresh = 1,
  Stale = 2,
  Expired = 3,
  Future = 4,
};

inline constexpr std::size_t kFreshnessClassCount = 4;

[[nodiscard]] std::string_view freshness_class_name(FreshnessClass value) noexcept;

struct FreshnessAssessment {
  FreshnessClass klass = FreshnessClass::Expired;
  Duration age{};
  // True when the evidence may contribute to accounting totals under the
  // supplied policy. Absence of a usable measurement is never treated as zero.
  bool usable = false;
  UnknownReason reason = UnknownReason::NoEvidence;
  std::string detail;
};

// Deterministic classification of one observation relative to as_of.
[[nodiscard]] FreshnessAssessment assess_freshness(const Observation& observation, TimePoint as_of,
                                                   const FreshnessPolicy& policy,
                                                   StaleEvidencePolicy stale_policy);

// Full admissibility assessment: freshness, incarnation coverage, generation
// binding, epoch and sequence fencing, and provenance support. accepted is the
// only signal a caller needs for "may this contribute".
struct AdmissibilityAssessment {
  bool accepted = false;
  FreshnessAssessment freshness{};
  UnknownReason reason = UnknownReason::None;
  std::string detail;
};

[[nodiscard]] AdmissibilityAssessment assess_admissibility(const Observation& observation,
                                                           TimePoint as_of,
                                                           const LedgerPolicy& policy,
                                                           const FabricTopology& topology,
                                                           const Source& source,
                                                           const SourceIncarnation* incarnation);

}  // namespace fel
