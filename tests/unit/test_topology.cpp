// Fabric Efficiency Ledger - topology structure and validation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/topology.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, fixture_topology_validates_and_answers_hierarchy_queries) {
  const auto fixture = feltest::make_fixture();
  FEL_ASSERT_OK(fixture.topology.validate());

  const auto children = fixture.topology.children_of(fixture.fabric_scope);
  FEL_REQUIRE(children.size() == 1);
  FEL_EXPECT_EQ(children.front(), fixture.pod_scope);

  const auto descendants = fixture.topology.descendants_of(fixture.fabric_scope);
  FEL_EXPECT_EQ(descendants.size(), std::size_t{4});

  const auto ancestors = fixture.topology.ancestors_of(fixture.port1_scope);
  FEL_EXPECT_EQ(ancestors.size(), std::size_t{3});
  FEL_EXPECT(std::find(ancestors.begin(), ancestors.end(), fixture.fabric_scope) !=
             ancestors.end());

  const auto direct = fixture.topology.resources_of(fixture.port1_scope, false);
  FEL_REQUIRE(direct.size() == 1);
  FEL_EXPECT_EQ(direct.front(), fixture.port1);

  const auto below_switch = fixture.topology.resources_of(fixture.switch_scope, true);
  FEL_EXPECT_EQ(below_switch.size(), std::size_t{2});
  FEL_EXPECT(below_switch != fixture.topology.resources_of(fixture.switch_scope, false));
}

FEL_TEST(unit, generation_binding_is_interval_bounded) {
  const auto fixture = feltest::make_fixture();
  FEL_EXPECT(fixture.topology.generation_binds(fixture.port1, fixture.generation,
                                               fixture.period_start));
  FEL_EXPECT(!fixture.topology.generation_binds(fixture.port1, fixture.superseded_generation,
                                                fixture.period_start));
  const TimePoint before = TimePoint::from_seconds(1600000000);
  FEL_EXPECT(!fixture.topology.generation_binds(fixture.port1, fixture.generation, before));
}

FEL_TEST(unit, validate_rejects_a_scope_cycle) {
  const ScopeId first = ScopeId::derive({"scope", "cycle-a"});
  const ScopeId second = ScopeId::derive({"scope", "cycle-b"});
  FabricTopology topology;
  Scope a;
  a.id = first;
  a.label = "a";
  a.kind = ScopeKind::Other;
  a.parent = second;
  FEL_ASSERT_OK(topology.add_scope(a));
  Scope b;
  b.id = second;
  b.label = "b";
  b.kind = ScopeKind::Other;
  b.parent = first;
  FEL_ASSERT_OK(topology.add_scope(b));
  FEL_ASSERT_ERR(topology.validate(), ErrorCode::ScopeCycleDetected);
}

FEL_TEST(unit, validate_rejects_a_resource_in_two_scopes) {
  auto fixture = feltest::make_fixture();
  Scope extra;
  extra.id = ScopeId::derive({"scope", "extra"});
  extra.label = "extra";
  extra.kind = ScopeKind::Other;
  extra.parent = fixture.switch_scope;
  extra.members = {fixture.port1};
  FEL_ASSERT_OK(fixture.topology.add_scope(extra));
  FEL_ASSERT_ERR(fixture.topology.validate(), ErrorCode::ResourceInMultipleScopes);
}

FEL_TEST(unit, validate_rejects_overlapping_incarnations) {
  auto fixture = feltest::make_fixture();
  SourceIncarnation second;
  second.id = IncarnationId::derive({"incarnation", "switch-a", "boot-2"});
  second.source = fixture.counter_source;
  second.boot_epoch = EpochId{2};
  second.started_at = fixture.period_start;
  FEL_ASSERT_OK(fixture.topology.add_incarnation(second));
  FEL_ASSERT_ERR(fixture.topology.validate(), ErrorCode::OverlappingIncarnations);
}

FEL_TEST(unit, validate_rejects_a_binding_to_an_unknown_generation) {
  FabricTopology topology;
  GenerationBinding binding;
  binding.resource = ResourceId::derive({"resource", "x"});
  binding.generation = GenerationId::derive({"generation", "missing"});
  binding.valid_from = TimePoint::from_seconds(1);
  FEL_ASSERT_OK(topology.add_binding(binding));
  FEL_ASSERT_ERR(topology.validate(), ErrorCode::UnknownResource);
}

FEL_TEST(unit, validate_rejects_a_resource_with_no_scope) {
  FabricTopology topology;
  Resource resource;
  resource.id = ResourceId::derive({"resource", "orphan"});
  resource.label = "orphan";
  resource.scope = ScopeId::derive({"scope", "nowhere"});
  FEL_ASSERT_OK(topology.add_resource(resource));
  FEL_ASSERT_ERR(topology.validate(), ErrorCode::UnknownScope);
}

FEL_TEST(unit, duplicate_definitions_are_refused) {
  auto fixture = feltest::make_fixture();
  Source duplicate;
  duplicate.id = fixture.counter_source;
  duplicate.label = "again";
  FEL_ASSERT_ERR(fixture.topology.add_source(duplicate), ErrorCode::DuplicateDefinition);

  Scope duplicate_scope;
  duplicate_scope.id = fixture.fabric_scope;
  FEL_ASSERT_ERR(fixture.topology.add_scope(duplicate_scope), ErrorCode::DuplicateDefinition);
}

FEL_TEST(unit, topology_digest_is_stable_and_content_addressed) {
  const auto first = feltest::make_fixture();
  const auto second = feltest::make_fixture();
  FEL_EXPECT_EQ(first.topology.digest().to_hex(), second.topology.digest().to_hex());
  FEL_EXPECT_EQ(first.topology.revision_id(), second.topology.revision_id());

  auto changed = feltest::make_fixture();
  Source extra;
  extra.id = SourceId::derive({"source", "extra"});
  extra.label = "extra";
  extra.kind = SourceKind::ManualEntry;
  extra.authority = AuthorityRank{1};
  FEL_ASSERT_OK(changed.topology.add_source(extra));
  FEL_EXPECT(changed.topology.digest() != first.topology.digest());
  FEL_EXPECT(changed.topology.revision_id() != first.topology.revision_id());
}

FEL_TEST(unit, enum_names_round_trip) {
  for (std::size_t i = 1; i <= kProvenanceClassCount; ++i) {
    const auto value = static_cast<ProvenanceClass>(i);
    FEL_REQUIRE(provenance_class_from_name(provenance_class_name(value)).has_value());
    FEL_EXPECT_EQ(provenance_class_from_name(provenance_class_name(value)).value(), value);
  }
  for (std::size_t i = 1; i <= kScopeKindCount; ++i) {
    const auto value = static_cast<ScopeKind>(i);
    FEL_REQUIRE(scope_kind_from_name(scope_kind_name(value)).has_value());
  }
  for (std::size_t i = 1; i <= 9; ++i) {
    const auto value = static_cast<SourceKind>(i);
    FEL_REQUIRE(source_kind_from_name(source_kind_name(value)).has_value());
  }
  for (std::size_t i = 1; i <= 6; ++i) {
    const auto value = static_cast<ResourceKind>(i);
    FEL_REQUIRE(resource_kind_from_name(resource_kind_name(value)).has_value());
  }
  FEL_EXPECT(!provenance_class_from_name("not-a-class").has_value());
  FEL_EXPECT(provenance_is_countable(ProvenanceClass::Real));
  FEL_EXPECT(provenance_is_countable(ProvenanceClass::Synthetic));
  FEL_EXPECT(!provenance_is_countable(ProvenanceClass::Unsupported));
}
