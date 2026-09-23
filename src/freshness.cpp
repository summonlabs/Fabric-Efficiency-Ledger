// Fabric Efficiency Ledger - evidence freshness and admissibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/freshness.hpp"

#include <string>

namespace fel {

std::string_view freshness_class_name(FreshnessClass value) noexcept {
  switch (value) {
    case FreshnessClass::Fresh:
      return "fresh";
    case FreshnessClass::Stale:
      return "stale";
    case FreshnessClass::Expired:
      return "expired";
    case FreshnessClass::Future:
      return "future";
  }
  return "invalid";
}

FreshnessAssessment assess_freshness(const Observation& observation, TimePoint as_of,
                                     const FreshnessPolicy& policy,
                                     StaleEvidencePolicy stale_policy) {
  FreshnessAssessment assessment;

  // A record whose observation time lies after its own receive time is either a
  // producer bug or a clock anomaly. Either way it is not usable evidence.
  if (observation.observed_at > observation.received_at) {
    assessment.klass = FreshnessClass::Future;
    assessment.usable = false;
    assessment.reason = UnknownReason::ClockAnomaly;
    assessment.detail = "observation time is later than receive time";
    assessment.age = Duration{};
    return assessment;
  }

  const TimePoint skew_bound = as_of + policy.clock_skew_allowance;
  if (observation.observed_at > skew_bound || observation.received_at > skew_bound) {
    assessment.klass = FreshnessClass::Future;
    assessment.age = Duration{};
    if (policy.reject_future_evidence) {
      assessment.usable = false;
      assessment.reason = UnknownReason::ClockAnomaly;
      assessment.detail = "evidence is dated after the evaluation instant";
      return assessment;
    }
    assessment.usable = true;
    assessment.reason = UnknownReason::None;
    assessment.detail = "future dated evidence accepted by policy";
    return assessment;
  }

  const Duration age = as_of - observation.observed_at;
  assessment.age = age.is_negative() ? Duration{} : age;

  if (assessment.age <= policy.fresh_ttl) {
    assessment.klass = FreshnessClass::Fresh;
    assessment.usable = true;
    assessment.reason = UnknownReason::None;
    return assessment;
  }
  if (assessment.age <= policy.stale_ttl) {
    assessment.klass = FreshnessClass::Stale;
    assessment.usable = stale_policy == StaleEvidencePolicy::IncludeMarked;
    assessment.reason = UnknownReason::StaleEvidence;
    assessment.detail = "evidence is older than the freshness budget";
    return assessment;
  }
  assessment.klass = FreshnessClass::Expired;
  assessment.usable = false;
  assessment.reason = UnknownReason::ExpiredEvidence;
  assessment.detail = "evidence is older than the retention budget";
  return assessment;
}

AdmissibilityAssessment assess_admissibility(const Observation& observation, TimePoint as_of,
                                             const LedgerPolicy& policy,
                                             const FabricTopology& topology, const Source& source,
                                             const SourceIncarnation* incarnation) {
  AdmissibilityAssessment result;

  if (!provenance_is_countable(source.provenance)) {
    result.accepted = false;
    result.reason = UnknownReason::UnsupportedProvenance;
    result.detail = std::string("source provenance class is not supported: ") +
                    std::string(provenance_class_name(source.provenance));
    return result;
  }
  if (!source.enabled) {
    result.accepted = false;
    result.reason = UnknownReason::UnsupportedProvenance;
    result.detail = "source is disabled";
    return result;
  }

  if (policy.freshness.require_incarnation_coverage) {
    if (incarnation == nullptr) {
      result.accepted = false;
      result.reason = UnknownReason::RetiredIncarnation;
      result.detail = "evidence names a source incarnation that is not defined";
      return result;
    }
    if (incarnation->source != observation.source) {
      result.accepted = false;
      result.reason = UnknownReason::AttributionFailure;
      result.detail = "incarnation does not belong to the named source";
      return result;
    }
    if (incarnation->retired_at.has_value() && observation.received_at > *incarnation->retired_at) {
      result.accepted = false;
      result.reason = UnknownReason::RetiredIncarnation;
      result.detail = "evidence arrived after the source incarnation was retired";
      return result;
    }
    if (observation.received_at < incarnation->started_at) {
      result.accepted = false;
      result.reason = UnknownReason::ClockAnomaly;
      result.detail = "evidence arrived before the source incarnation started";
      return result;
    }
  }

  if (incarnation != nullptr && observation.epoch < incarnation->boot_epoch) {
    result.accepted = false;
    result.reason = UnknownReason::FencedEpoch;
    result.detail = "evidence epoch precedes the boot epoch of its incarnation";
    return result;
  }

  if (policy.freshness.require_generation_binding && observation.resource.has_value()) {
    if (!topology.generation_binds(*observation.resource, observation.generation,
                                   observation.observed_at)) {
      result.accepted = false;
      result.reason = UnknownReason::StaleGeneration;
      result.detail = "no generation binding relates this resource to the reported generation";
      return result;
    }
  }

  result.freshness = assess_freshness(observation, as_of, policy.freshness, policy.stale);
  if (!result.freshness.usable) {
    result.accepted = false;
    result.reason = result.freshness.reason;
    result.detail = result.freshness.detail;
    return result;
  }
  result.accepted = true;
  result.reason = UnknownReason::None;
  return result;
}

}  // namespace fel
