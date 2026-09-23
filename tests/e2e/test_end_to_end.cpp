// Fabric Efficiency Ledger - end to end accounting behaviour.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

struct Rig {
  feltest::Fixture fixture;
  std::unique_ptr<Ledger> ledger;
  PeriodId period;
};

[[nodiscard]] Result<Rig> make_rig() {
  Rig rig;
  rig.fixture = feltest::make_fixture();
  auto ledger = Ledger::in_memory(feltest::test_policy(), rig.fixture.topology,
                                  rig.fixture.period_end);
  if (!ledger.has_value()) {
    return ledger.error();
  }
  rig.ledger = std::move(ledger.value());
  auto period = rig.ledger->open_period(rig.fixture.period_start, rig.fixture.period_end, "hour-1");
  if (!period.has_value()) {
    return period.error();
  }
  rig.period = period.value();
  return rig;
}

[[nodiscard]] Observation useful(const feltest::Fixture& fixture, std::uint64_t sequence,
                                 std::uint64_t amount, std::optional<ResourceId> resource) {
  feltest::ObservationSpec spec;
  spec.source = fixture.counter_source;
  spec.incarnation = fixture.counter_incarnation;
  spec.sequence = sequence;
  spec.generation = fixture.generation;
  spec.amount = amount;
  spec.category = Category::UsefulDeliveredWork;
  spec.resource = resource;
  spec.observed_at = fixture.period_start + Duration::from_minutes(5);
  spec.received_at = spec.observed_at + Duration::from_seconds(1);
  spec.window_start = fixture.period_start;
  spec.window_end = fixture.period_start + Duration::from_minutes(10);
  spec.label = "useful-" + std::to_string(sequence);
  return feltest::make_observation(spec);
}

}  // namespace

FEL_TEST(e2e, ingest_close_query_roundtrip) {
  auto rig_result = make_rig();
  FEL_REQUIRE(rig_result.has_value());
  Rig rig = std::move(rig_result.value());

  const auto first = useful(rig.fixture, 1, 1000, rig.fixture.port1);
  const auto second = useful(rig.fixture, 2, 2500, rig.fixture.port2);
  const auto batch = feltest::make_batch({first, second});
  auto report = rig.ledger->ingest(batch);
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().accepted, std::uint64_t{2});
  FEL_EXPECT_EQ(report.value().rejected_shape, std::uint64_t{0});

  auto closed = rig.ledger->close_period(CloseRequest{rig.period, rig.fixture.period_end, "close",
                                                      "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(closed.value().claims.size(), std::size_t{2});
  FEL_EXPECT_EQ(closed.value().evidence_admitted, std::uint64_t{2});

  QueryRequest request;
  request.period = rig.period;
  auto summary = rig.ledger->query(request);
  FEL_REQUIRE(summary.has_value());
  FEL_EXPECT_EQ(summary.value().rows.size(), std::size_t{2});
  FEL_EXPECT_EQ(summary.value().state, PeriodState::Closed);

  std::uint64_t total = 0;
  for (const auto& row : summary.value().rows) {
    total += row.cell.known_total;
  }
  FEL_EXPECT_EQ(total, std::uint64_t{3500});
}

FEL_TEST(e2e, unknown_stays_unknown_when_no_evidence_exists) {
  auto rig_result = make_rig();
  FEL_REQUIRE(rig_result.has_value());
  Rig rig = std::move(rig_result.value());

  auto closed = rig.ledger->close_period(CloseRequest{rig.period, rig.fixture.period_end, "close",
                                                      "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(closed.value().claims.size(), std::size_t{0});
  FEL_EXPECT_EQ(closed.value().evidence_considered, std::uint64_t{0});
  for (const auto& summary : closed.value().efficiency) {
    FEL_EXPECT(!summary.determinate);
    FEL_EXPECT_EQ(summary.denominator, std::uint64_t{0});
  }
}

FEL_TEST(e2e, unused_source_is_recorded_but_never_counted) {
  auto rig_result = make_rig();
  FEL_REQUIRE(rig_result.has_value());
  Rig rig = std::move(rig_result.value());

  feltest::ObservationSpec spec;
  spec.source = rig.fixture.unsupported_source;
  spec.incarnation = rig.fixture.counter_incarnation;
  spec.generation = rig.fixture.generation;
  spec.amount = 999999;
  spec.resource = rig.fixture.port1;
  spec.observed_at = rig.fixture.period_start + Duration::from_minutes(1);
  spec.received_at = spec.observed_at;
  spec.window_start = rig.fixture.period_start;
  spec.window_end = rig.fixture.period_end;
  spec.label = "unsupported";

  auto observation = feltest::make_observation(spec);
  auto report = rig.ledger->ingest(feltest::make_batch({observation}));
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT_EQ(report.value().rejected_unsupported_provenance, std::uint64_t{1});

  auto closed = rig.ledger->close_period(CloseRequest{rig.period, rig.fixture.period_end, "close",
                                                      "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(closed.value().claims.size(), std::size_t{0});
  FEL_EXPECT_EQ(closed.value().evidence_admitted, std::uint64_t{0});
}
