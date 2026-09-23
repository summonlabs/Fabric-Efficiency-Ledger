// Fabric Efficiency Ledger - seeded randomized invariant checking.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

constexpr std::size_t kSeeds = 12;
constexpr std::size_t kRecordsPerSeed = 120;

struct Generated {
  std::vector<Observation> observations;
};

[[nodiscard]] Generated generate(std::uint64_t seed, const feltest::Fixture& fixture) {
  XorShift64 rng(seed);
  Generated generated;
  for (std::size_t i = 0; i < kRecordsPerSeed; ++i) {
    const bool synthetic = rng.below(3) == 0;
    const auto source = synthetic ? fixture.probe_source : fixture.counter_source;
    const auto incarnation = synthetic ? fixture.probe_incarnation : fixture.counter_incarnation;
    const auto resource = rng.below(2) == 0 ? fixture.port1 : fixture.port2;
    const auto category = static_cast<Category>(1 + rng.below(kCategoryCount));
    auto spec = feltest::make_spec(fixture, source, incarnation, i + 1, rng.below(10000) + 1,
                                   category, resource);
    // Distinct reporting windows keep independent records independent.
    const auto offset = Duration::from_minutes(static_cast<std::int64_t>(rng.below(50)));
    spec.window_start = fixture.period_start + offset;
    spec.window_end = spec.window_start + Duration::from_minutes(1);
    spec.observed_at = spec.window_end;
    spec.received_at = spec.window_end;
    generated.observations.push_back(feltest::make_observation(spec));
  }
  return generated;
}

void check_invariants(const PeriodRevision& revision) {
  // Every claim's identity is reproducible from its key.
  for (const auto& claim : revision.claims) {
    FEL_EXPECT_EQ(claim.identity.to_string(), claim.key.identity().to_string());
    FEL_EXPECT(claim.cell.kind == claim.key.kind);
    FEL_EXPECT(claim.evidence.size() <= claim.cell.known_contributions ||
               claim.cell.known_contributions == 0);
    FEL_EXPECT(claim.cell.coverage() != Coverage::Known ||
               claim.cell.unknown_contributions == 0);
  }
  // Claims are strictly ordered and unique per key.
  for (std::size_t i = 1; i < revision.claims.size(); ++i) {
    FEL_EXPECT(revision.claims[i - 1].key < revision.claims[i].key ||
               !(revision.claims[i].key < revision.claims[i - 1].key));
    FEL_EXPECT(!(revision.claims[i].key == revision.claims[i - 1].key));
  }
  // The unknown bucket only ever carries an explicit reason.
  for (const auto& unknown : revision.unknowns) {
    FEL_EXPECT(unknown.reason != UnknownReason::None);
    FEL_EXPECT(unknown.evidence_count > 0);
  }
  // Conservation arithmetic is self consistent inside each domain.
  for (const auto& domain : revision.conservation) {
    if (domain.status == ConservationStatus::Residual) {
      FEL_REQUIRE(domain.observed_total.has_value());
      FEL_EXPECT_EQ(domain.attributed_total + domain.residual_to_unknown, *domain.observed_total);
    }
    if (domain.status == ConservationStatus::Consistent) {
      FEL_REQUIRE(domain.observed_total.has_value());
      FEL_EXPECT_EQ(domain.attributed_total, *domain.observed_total);
    }
    if (domain.status == ConservationStatus::Unverifiable) {
      FEL_EXPECT(!domain.observed_total.has_value());
    }
  }
  // Efficiency only ever reports determinate ratios with a positive denominator.
  for (const auto& summary : revision.efficiency) {
    if (summary.determinate) {
      FEL_EXPECT(summary.denominator > 0);
      FEL_EXPECT(summary.ratio_denominator > 0);
      FEL_EXPECT(summary.ratio_permille <= 1000);
      FEL_EXPECT(summary.useful <= summary.denominator);
    } else {
      FEL_EXPECT(!summary.indeterminacy_reason.empty());
    }
    FEL_EXPECT_EQ(summary.denominator, summary.useful + summary.necessary_overhead +
                                           summary.avoidable + summary.failed);
  }
  // Evidence accounting closes: admitted plus rejected is the considered total.
  FEL_EXPECT_EQ(revision.evidence_admitted + revision.evidence_rejected,
                revision.evidence_considered);
}

}  // namespace

FEL_TEST(property, randomized_evidence_preserves_accounting_invariants) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    auto rig_result = feltest::make_memory_rig();
    FEL_REQUIRE(rig_result.has_value());
    auto rig = std::move(rig_result.value());
    const auto generated = generate(seed, rig.fixture);
    FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(generated.observations)));
    auto closed = rig.ledger->close_period(
        CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
    FEL_REQUIRE(closed.has_value());
    check_invariants(closed.value());
    FEL_ASSERT_OK(closed.value().verify_digest());
  }
}

FEL_TEST(property, claim_totals_equal_the_sum_of_admitted_contributions) {
  for (std::uint64_t seed = 100; seed < 100 + kSeeds; ++seed) {
    auto rig_result = feltest::make_memory_rig();
    FEL_REQUIRE(rig_result.has_value());
    auto rig = std::move(rig_result.value());
    const auto generated = generate(seed, rig.fixture);
    FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(generated.observations)));
    auto closed = rig.ledger->close_period(
        CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
    FEL_REQUIRE(closed.has_value());

    std::uint64_t domain_sum = 0;
    for (const auto& domain : closed.value().conservation) {
      domain_sum += domain.attributed_total;
    }
    // Domains are disjoint partitions of the same contributions, so their sum
    // must equal the sum over claims.
    std::uint64_t claim_sum = 0;
    for (const auto& claim : closed.value().claims) {
      claim_sum += claim.cell.known_total;
    }
    FEL_EXPECT_EQ(claim_sum, domain_sum);
  }
}

FEL_TEST(property, no_observation_is_counted_twice_in_a_randomized_ledger) {
  for (std::uint64_t seed = 200; seed < 200 + kSeeds; ++seed) {
    auto rig_result = feltest::make_memory_rig();
    FEL_REQUIRE(rig_result.has_value());
    auto rig = std::move(rig_result.value());
    const auto generated = generate(seed, rig.fixture);
    FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(generated.observations)));
    // Ingesting the identical batch again must add nothing at all.
    auto second = rig.ledger->ingest(feltest::make_batch(generated.observations));
    FEL_REQUIRE(second.has_value());
    FEL_EXPECT_EQ(second.value().accepted, std::uint64_t{0});

    auto first_close = rig.ledger->close_period(
        CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
    FEL_REQUIRE(first_close.has_value());
    check_invariants(first_close.value());
  }
}
