// Fabric Efficiency Ledger - freshness classification and admissibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fel/freshness.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

struct Fixture2 {
  feltest::Fixture fixture;
  Observation observation;
  FreshnessPolicy policy;
};

[[nodiscard]] Fixture2 make_case() {
  Fixture2 setup;
  setup.fixture = feltest::make_fixture();
  setup.observation = feltest::make_observation(feltest::make_spec(
      setup.fixture, setup.fixture.counter_source, setup.fixture.counter_incarnation, 1, 100,
      Category::UsefulDeliveredWork, setup.fixture.port1));
  setup.policy.fresh_ttl = Duration::from_minutes(5);
  setup.policy.stale_ttl = Duration::from_minutes(30);
  setup.policy.clock_skew_allowance = Duration::from_seconds(5);
  return setup;
}

}  // namespace

FEL_TEST(unit, freshness_classes_follow_the_as_of_instant) {
  Fixture2 setup = make_case();
  const TimePoint observed = setup.observation.observed_at;

  auto fresh = assess_freshness(setup.observation, observed + Duration::from_minutes(1),
                                setup.policy, StaleEvidencePolicy::Exclude);
  FEL_EXPECT_EQ(fresh.klass, FreshnessClass::Fresh);
  FEL_EXPECT(fresh.usable);

  auto stale = assess_freshness(setup.observation, observed + Duration::from_minutes(10),
                                setup.policy, StaleEvidencePolicy::Exclude);
  FEL_EXPECT_EQ(stale.klass, FreshnessClass::Stale);
  FEL_EXPECT(!stale.usable);
  FEL_EXPECT_EQ(stale.reason, UnknownReason::StaleEvidence);

  auto expired = assess_freshness(setup.observation, observed + Duration::from_hours(2),
                                  setup.policy, StaleEvidencePolicy::Exclude);
  FEL_EXPECT_EQ(expired.klass, FreshnessClass::Expired);
  FEL_EXPECT(!expired.usable);
  FEL_EXPECT_EQ(expired.reason, UnknownReason::ExpiredEvidence);
}

FEL_TEST(unit, include_marked_makes_stale_evidence_usable) {
  Fixture2 setup = make_case();
  const TimePoint as_of = setup.observation.observed_at + Duration::from_minutes(10);
  auto included = assess_freshness(setup.observation, as_of, setup.policy,
                                   StaleEvidencePolicy::IncludeMarked);
  FEL_EXPECT_EQ(included.klass, FreshnessClass::Stale);
  FEL_EXPECT(included.usable);
}

FEL_TEST(unit, future_dated_evidence_is_a_clock_anomaly) {
  Fixture2 setup = make_case();
  const TimePoint as_of = setup.observation.observed_at - Duration::from_hours(1);
  auto assessment =
      assess_freshness(setup.observation, as_of, setup.policy, StaleEvidencePolicy::Exclude);
  FEL_EXPECT_EQ(assessment.klass, FreshnessClass::Future);
  FEL_EXPECT(!assessment.usable);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::ClockAnomaly);
}

FEL_TEST(unit, observation_after_receive_time_is_a_clock_anomaly) {
  Fixture2 setup = make_case();
  Observation inverted = setup.observation;
  inverted.observed_at = inverted.received_at + Duration::from_seconds(1);
  auto assessment = assess_freshness(inverted, inverted.observed_at, setup.policy,
                                     StaleEvidencePolicy::Exclude);
  FEL_EXPECT_EQ(assessment.klass, FreshnessClass::Future);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::ClockAnomaly);
  FEL_EXPECT(!assessment.usable);
}

FEL_TEST(unit, admissibility_rejects_an_unsupported_provenance) {
  Fixture2 setup = make_case();
  Observation observation = setup.observation;
  observation.source = setup.fixture.unsupported_source;
  const Source* source = setup.fixture.topology.find_source(observation.source);
  FEL_REQUIRE(source != nullptr);
  auto assessment =
      assess_admissibility(observation, observation.observed_at, LedgerPolicy{},
                           setup.fixture.topology, *source,
                           setup.fixture.topology.find_incarnation(observation.incarnation));
  FEL_EXPECT(!assessment.accepted);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::UnsupportedProvenance);
}

FEL_TEST(unit, admissibility_rejects_a_retired_incarnation) {
  Fixture2 setup = make_case();
  Observation observation = setup.observation;
  SourceIncarnation incarnation = *setup.fixture.topology.find_incarnation(
      observation.incarnation);
  incarnation.retired_at = observation.received_at - Duration::from_seconds(1);
  const Source* source = setup.fixture.topology.find_source(observation.source);
  FEL_REQUIRE(source != nullptr);
  auto assessment = assess_admissibility(observation, observation.observed_at, LedgerPolicy{},
                                         setup.fixture.topology, *source, &incarnation);
  FEL_EXPECT(!assessment.accepted);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::RetiredIncarnation);
}

FEL_TEST(unit, admissibility_rejects_evidence_from_before_the_incarnation_started) {
  Fixture2 setup = make_case();
  Observation observation = setup.observation;
  SourceIncarnation incarnation = *setup.fixture.topology.find_incarnation(
      observation.incarnation);
  incarnation.started_at = observation.received_at + Duration::from_seconds(1);
  const Source* source = setup.fixture.topology.find_source(observation.source);
  FEL_REQUIRE(source != nullptr);
  auto assessment = assess_admissibility(observation, observation.observed_at, LedgerPolicy{},
                                         setup.fixture.topology, *source, &incarnation);
  FEL_EXPECT(!assessment.accepted);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::ClockAnomaly);
}

FEL_TEST(unit, admissibility_rejects_a_missing_generation_binding) {
  Fixture2 setup = make_case();
  Observation observation = setup.observation;
  observation.generation = setup.fixture.superseded_generation;
  const Source* source = setup.fixture.topology.find_source(observation.source);
  FEL_REQUIRE(source != nullptr);
  auto assessment =
      assess_admissibility(observation, observation.observed_at, LedgerPolicy{},
                           setup.fixture.topology, *source,
                           setup.fixture.topology.find_incarnation(observation.incarnation));
  FEL_EXPECT(!assessment.accepted);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::StaleGeneration);
}

FEL_TEST(unit, admissibility_accepts_a_well_formed_observation) {
  Fixture2 setup = make_case();
  const Source* source = setup.fixture.topology.find_source(setup.observation.source);
  FEL_REQUIRE(source != nullptr);
  auto assessment = assess_admissibility(
      setup.observation, setup.observation.observed_at, LedgerPolicy{}, setup.fixture.topology,
      *source, setup.fixture.topology.find_incarnation(setup.observation.incarnation));
  FEL_EXPECT(assessment.accepted);
  FEL_EXPECT_EQ(assessment.reason, UnknownReason::None);
  FEL_EXPECT_EQ(assessment.freshness.klass, FreshnessClass::Fresh);
}
