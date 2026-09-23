// Fabric Efficiency Ledger - measures, categories and claim identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <string>
#include <vector>

#include "fel/claim.hpp"
#include "fel/measure.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, measure_kind_names_round_trip) {
  for (std::size_t i = 1; i <= kMeasureKindCount; ++i) {
    const auto kind = static_cast<MeasureKind>(i);
    const auto parsed = measure_kind_from_name(measure_kind_name(kind));
    FEL_REQUIRE(parsed.has_value());
    FEL_EXPECT_EQ(parsed.value(), kind);
    FEL_EXPECT(!measure_kind_unit(kind).empty());
  }
  FEL_EXPECT(!measure_kind_from_name("octets").has_value());
  FEL_EXPECT_EQ(std::string(measure_kind_name(static_cast<MeasureKind>(0))), std::string("invalid"));
}

FEL_TEST(unit, category_names_round_trip_and_usefulness_is_explicit) {
  for (std::size_t i = 1; i <= kCategoryCount; ++i) {
    const auto category = static_cast<Category>(i);
    const auto parsed = category_from_name(category_name(category));
    FEL_REQUIRE(parsed.has_value());
    FEL_EXPECT_EQ(parsed.value(), category);
  }
  FEL_EXPECT_EQ(usefulness_of(Category::UsefulDeliveredWork), Usefulness::Useful);
  FEL_EXPECT_EQ(usefulness_of(Category::ControlOverhead), Usefulness::NecessaryOverhead);
  FEL_EXPECT_EQ(usefulness_of(Category::Retransmission), Usefulness::Avoidable);
  FEL_EXPECT_EQ(usefulness_of(Category::Duplication), Usefulness::Avoidable);
  FEL_EXPECT_EQ(usefulness_of(Category::RerouteOverhead), Usefulness::Avoidable);
  FEL_EXPECT_EQ(usefulness_of(Category::IdleReservation), Usefulness::Avoidable);
  FEL_EXPECT_EQ(usefulness_of(Category::StrandedCapacity), Usefulness::Avoidable);
  FEL_EXPECT_EQ(usefulness_of(Category::FailedTransfer), Usefulness::Failed);
  // The unknown bucket is never classified as useful work.
  FEL_EXPECT_EQ(usefulness_of(Category::UnknownUnattributed), Usefulness::Unknown);
  FEL_EXPECT(!category_from_name("invented").has_value());
}

FEL_TEST(unit, unknown_reasons_round_trip) {
  for (std::size_t i = 0; i < kUnknownReasonCount; ++i) {
    const auto reason = static_cast<UnknownReason>(i);
    const auto parsed = unknown_reason_from_name(unknown_reason_name(reason));
    FEL_REQUIRE(parsed.has_value());
    FEL_EXPECT_EQ(parsed.value(), reason);
  }
  FEL_EXPECT_EQ(unknown_reason_name(UnknownReason::None), std::string("none"));
  FEL_EXPECT(!unknown_reason_from_name("made-up").has_value());
}

FEL_TEST(unit, coverage_names_are_stable) {
  FEL_EXPECT_EQ(coverage_name(Coverage::Known), std::string("known"));
  FEL_EXPECT_EQ(coverage_name(Coverage::Partial), std::string("partial"));
  FEL_EXPECT_EQ(coverage_name(Coverage::Unknown), std::string("unknown"));
}

FEL_TEST(unit, claim_key_identity_is_stable_and_field_sensitive) {
  ClaimKey key;
  key.generation = GenerationId::derive({"generation", "g1"});
  key.scope = ScopeId::derive({"scope", "port-1"});
  key.kind = MeasureKind::WireBytes;
  key.category = Category::UsefulDeliveredWork;
  key.basis = AttributionBasis::ResourceDirect;
  key.resource = ResourceId::derive({"resource", "port-1"});

  const ClaimKey copy = key;
  FEL_EXPECT_EQ(key.identity(), copy.identity());
  FEL_EXPECT_EQ(key.canonical_form(), copy.canonical_form());

  ClaimKey changed = key;
  changed.category = Category::Retransmission;
  FEL_EXPECT(key.identity() != changed.identity());

  changed = key;
  changed.kind = MeasureKind::PayloadBytes;
  FEL_EXPECT(key.identity() != changed.identity());

  changed = key;
  changed.resource = ResourceId::derive({"resource", "port-2"});
  FEL_EXPECT(key.identity() != changed.identity());

  changed = key;
  changed.basis = AttributionBasis::PathDirect;
  FEL_EXPECT(key.identity() != changed.identity());
}

FEL_TEST(unit, claim_key_ordering_is_a_strict_weak_ordering) {
  std::vector<ClaimKey> keys;
  for (int i = 0; i < 12; ++i) {
    ClaimKey key;
    key.generation = GenerationId::derive({"generation", "g1"});
    key.scope = ScopeId::derive({"scope", std::to_string(i % 3)});
    key.kind = static_cast<MeasureKind>(1 + (i % kMeasureKindCount));
    key.category = static_cast<Category>(1 + (i % kCategoryCount));
    key.basis = AttributionBasis::ResourceDirect;
    key.resource = ResourceId::derive({"resource", std::to_string(i)});
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    for (std::size_t j = 0; j < keys.size(); ++j) {
      FEL_EXPECT(!(keys[i] < keys[j] && keys[j] < keys[i]));
    }
  }
  FEL_EXPECT(std::is_sorted(keys.begin(), keys.end()));
  FEL_EXPECT_EQ(keys.front() == keys.front(), true);
}

FEL_TEST(unit, efficiency_ratio_text_is_explicit_about_indeterminacy) {
  EfficiencySummary determinate;
  determinate.determinate = true;
  determinate.useful = 3;
  determinate.denominator = 4;
  determinate.ratio_numerator = 3;
  determinate.ratio_denominator = 4;
  FEL_EXPECT_EQ(determinate.ratio_text(), std::string("3/4"));

  EfficiencySummary indeterminate;
  indeterminate.determinate = false;
  indeterminate.indeterminacy_reason = "some contributions could not be measured";
  FEL_EXPECT(indeterminate.ratio_text().find("indeterminate") == 0);
  FEL_EXPECT(indeterminate.ratio_text().find("could not be measured") != std::string::npos);
}

FEL_TEST(unit, attribution_basis_names_round_trip) {
  for (std::size_t i = 1; i <= kAttributionBasisCount; ++i) {
    FEL_EXPECT(!attribution_basis_name(static_cast<AttributionBasis>(i)).empty());
  }
  FEL_EXPECT_EQ(std::string(attribution_basis_name(AttributionBasis::Unattributed)),
                std::string("unattributed"));
}
