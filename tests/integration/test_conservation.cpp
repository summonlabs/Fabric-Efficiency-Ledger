// Fabric Efficiency Ledger - conservation, residuals and the unknown bucket.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] const DomainConservation* find_domain(const std::vector<DomainConservation>& domains,
                                                    const ScopeId& scope, MeasureKind kind) {
  for (const auto& domain : domains) {
    if (domain.scope == scope && domain.kind == kind) {
      return &domain;
    }
  }
  return nullptr;
}

[[nodiscard]] Observation total_observation(const feltest::Fixture& fixture, SourceId source,
                                            IncarnationId incarnation, std::uint64_t sequence,
                                            std::uint64_t amount,
                                            std::optional<ResourceId> resource) {
  auto spec = feltest::make_spec(fixture, source, incarnation, sequence, amount,
                                 Category::UnknownUnattributed, resource);
  spec.role = ObservationRole::Total;
  return feltest::make_observation(spec);
}

}  // namespace

FEL_TEST(integration, conservation_is_consistent_when_totals_agree) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto useful = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 700,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto retransmit = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 2, 300,
      Category::Retransmission, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 3, 1000, rig.fixture.port1);

  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({useful, retransmit, total})));
  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());

  const DomainConservation* domain =
      find_domain(closed.value().conservation, rig.fixture.port1_scope, MeasureKind::WireBytes);
  FEL_REQUIRE(domain != nullptr);
  FEL_EXPECT_EQ(domain->attributed_total, std::uint64_t{1000});
  FEL_REQUIRE(domain->observed_total.has_value());
  FEL_EXPECT_EQ(*domain->observed_total, std::uint64_t{1000});
  FEL_EXPECT_EQ(domain->status, ConservationStatus::Consistent);
  FEL_EXPECT(!closed.value().has_residual);
}

FEL_TEST(integration, residual_lands_in_the_explicit_unknown_bucket) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto useful = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 600,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 2, 1000, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({useful, total})));

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  const DomainConservation* domain =
      find_domain(closed.value().conservation, rig.fixture.port1_scope, MeasureKind::WireBytes);
  FEL_REQUIRE(domain != nullptr);
  FEL_EXPECT_EQ(domain->status, ConservationStatus::Residual);
  FEL_EXPECT_EQ(domain->residual_to_unknown, std::uint64_t{400});
  FEL_EXPECT(closed.value().has_residual);

  bool saw_residual = false;
  for (const auto& unknown : closed.value().unknowns) {
    if (unknown.reason == UnknownReason::ResidualUnclassified) {
      saw_residual = true;
    }
  }
  FEL_EXPECT(saw_residual);

  // The unattributed 400 is never silently added to a classification.
  for (const auto& claim : closed.value().claims) {
    FEL_EXPECT(claim.key.category != Category::UnknownUnattributed ||
               claim.cell.known_total == 0);
  }
}

FEL_TEST(integration, residual_policy_strict_reject_blocks_the_close) {
  auto policy = feltest::test_policy();
  policy.residual = ResidualPolicy::StrictReject;
  auto rig_result = feltest::make_memory_rig_with(policy);
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto useful = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 600,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 2, 1000, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({useful, total})));
  FEL_ASSERT_ERR(rig.ledger->close_period(
                     CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}),
                 ErrorCode::ConservationViolation);
}

FEL_TEST(integration, over_attribution_is_flagged_and_never_silently_reduced) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto useful = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 1200,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 2, 1000, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({useful, total})));

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  const DomainConservation* domain =
      find_domain(closed.value().conservation, rig.fixture.port1_scope, MeasureKind::WireBytes);
  FEL_REQUIRE(domain != nullptr);
  FEL_EXPECT_EQ(domain->status, ConservationStatus::Excess);
  FEL_EXPECT_EQ(domain->attributed_total, std::uint64_t{1200});
  FEL_EXPECT_EQ(*domain->observed_total, std::uint64_t{1000});
  FEL_EXPECT(closed.value().has_excess);
  bool saw_over = false;
  for (const auto& unknown : closed.value().unknowns) {
    if (unknown.reason == UnknownReason::OverAttribution) {
      saw_over = true;
    }
  }
  FEL_EXPECT(saw_over);
}

FEL_TEST(integration, conservation_never_mixes_measure_kinds) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  auto bytes = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                  rig.fixture.counter_incarnation, 1, 1000,
                                  Category::UsefulDeliveredWork, rig.fixture.port1);
  bytes.kind = MeasureKind::WireBytes;
  auto nanos = feltest::make_spec(rig.fixture, rig.fixture.counter_source,
                                  rig.fixture.counter_incarnation, 2, 5000,
                                  Category::UsefulDeliveredWork, rig.fixture.port1);
  nanos.kind = MeasureKind::PortOccupancyNanos;

  const auto total_bytes = total_observation(rig.fixture, rig.fixture.counter_source,
                                             rig.fixture.counter_incarnation, 3, 1000,
                                             rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch(
      {feltest::make_observation(bytes), feltest::make_observation(nanos), total_bytes})));

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());

  const DomainConservation* byte_domain =
      find_domain(closed.value().conservation, rig.fixture.port1_scope, MeasureKind::WireBytes);
  const DomainConservation* time_domain = find_domain(
      closed.value().conservation, rig.fixture.port1_scope, MeasureKind::PortOccupancyNanos);
  FEL_REQUIRE(byte_domain != nullptr);
  FEL_REQUIRE(time_domain != nullptr);
  FEL_EXPECT_EQ(byte_domain->attributed_total, std::uint64_t{1000});
  FEL_EXPECT_EQ(byte_domain->status, ConservationStatus::Consistent);
  // The nanosecond domain has no independent total, so it stays unverifiable
  // rather than borrowing the byte total.
  FEL_EXPECT_EQ(time_domain->attributed_total, std::uint64_t{5000});
  FEL_EXPECT_EQ(time_domain->status, ConservationStatus::Unverifiable);
  FEL_EXPECT(!time_domain->observed_total.has_value());
}

FEL_TEST(integration, conservation_domains_are_partitioned_by_generation) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());

  const auto observation = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 500,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 2, 500, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({observation, total})));

  auto closed = rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  for (const auto& domain : closed.value().conservation) {
    FEL_EXPECT_EQ(domain.generation, rig.fixture.generation);
  }
  FEL_EXPECT_EQ(closed.value().generations.size(), std::size_t{1});
}

FEL_TEST(integration, reconcile_reports_every_domain) {
  auto rig_result = feltest::make_memory_rig();
  FEL_REQUIRE(rig_result.has_value());
  auto rig = std::move(rig_result.value());
  const auto observation = feltest::make_observation(feltest::make_spec(
      rig.fixture, rig.fixture.counter_source, rig.fixture.counter_incarnation, 1, 500,
      Category::UsefulDeliveredWork, rig.fixture.port1));
  const auto total = total_observation(rig.fixture, rig.fixture.counter_source,
                                       rig.fixture.counter_incarnation, 2, 500, rig.fixture.port1);
  FEL_ASSERT_OK(rig.ledger->ingest(feltest::make_batch({observation, total})));
  FEL_ASSERT_OK(rig.ledger->close_period(
      CloseRequest{rig.period, rig.fixture.period_end, "close", "test", false}));

  ReconcileRequest request;
  request.period = rig.period;
  auto report = rig.ledger->reconcile(request);
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT(report.value().fully_consistent);
  FEL_EXPECT_EQ(report.value().domains_residual, std::uint64_t{0});
  FEL_EXPECT_EQ(report.value().domains_excess, std::uint64_t{0});
  FEL_EXPECT(!report.value().domains.empty());
}
