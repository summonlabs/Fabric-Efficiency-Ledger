// Fabric Efficiency Ledger - persistence, restart, and conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fel/ledger.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

struct StoreFixture {
  feltest::ScopedTempDir directory{"restart"};
  feltest::Fixture fixture = feltest::make_fixture();
};

[[nodiscard]] OpenOptions options_for(const StoreFixture& env, TimePoint as_of,
                                      LedgerPolicy policy) {
  OpenOptions options;
  options.store_path = env.directory.child("store");
  options.mode = StoreOpenMode::OpenOrCreate;
  options.policy = std::move(policy);
  options.topology = env.fixture.topology;
  options.as_of = as_of;
  // An explicit as_of keeps the test independent of the wall clock.
  options.rebase_freshness_on_open = false;
  return options;
}

// Each sample gets its own reporting window. Two records that share a cell and
// a window with different amounts is a conflict by design, not a sum.
[[nodiscard]] Observation sample(const feltest::Fixture& fixture, std::uint64_t sequence,
                                 std::uint64_t amount) {
  auto spec = feltest::make_spec(fixture, fixture.counter_source, fixture.counter_incarnation,
                                 sequence, amount, Category::UsefulDeliveredWork, fixture.port1);
  const auto offset = Duration::from_minutes(static_cast<std::int64_t>(sequence) * 2);
  spec.window_start = fixture.period_start + offset;
  spec.window_end = spec.window_start + Duration::from_minutes(1);
  spec.observed_at = spec.window_end;
  spec.received_at = spec.window_end;
  return feltest::make_observation(spec);
}

void append_bytes(const std::string& path, const std::string& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "ab");
  if (file != nullptr) {
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
  }
}

[[nodiscard]] std::uint64_t revision_total(const PeriodRevision& revision) {
  std::uint64_t total = 0;
  for (const auto& claim : revision.claims) {
    total += claim.cell.known_total;
  }
  return total;
}

}  // namespace

FEL_TEST(integration, evidence_and_periods_survive_a_restart) {
  StoreFixture env;
  std::uint64_t expected = 0;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
    FEL_REQUIRE(period.has_value());
    FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 111),
                                                              sample(env.fixture, 2, 222)})));
    auto closed = ledger.value()->close_period(
        CloseRequest{period.value(), env.fixture.period_end, "close", "test", false});
    FEL_REQUIRE(closed.has_value());
    expected = revision_total(closed.value());
    FEL_EXPECT_EQ(expected, std::uint64_t{333});
    FEL_ASSERT_OK(ledger.value()->flush());
  }
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto periods = ledger.value()->periods();
    FEL_REQUIRE(periods.size() == 1);
    FEL_EXPECT_EQ(periods.front().state, PeriodState::Closed);
    FEL_EXPECT_EQ(ledger.value()->retained_observation_count(), std::uint64_t{2});
    auto latest = ledger.value()->latest_revision(periods.front().id);
    FEL_REQUIRE(latest.has_value());
    FEL_EXPECT_EQ(revision_total(latest.value()), expected);
    FEL_ASSERT_OK(latest.value().verify_digest());
  }
}

FEL_TEST(integration, reopened_evidence_is_never_silently_promoted_to_fresh) {
  StoreFixture env;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
    FEL_REQUIRE(period.has_value());
    FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 500)})));
  }
  // Reopen far in the future with an explicit as_of. The evidence must be
  // expired, and the period must report unknown rather than the old total.
  const TimePoint far_future = env.fixture.period_end + Duration::from_days(30);
  auto ledger = Ledger::open(options_for(env, far_future, feltest::test_policy()));
  FEL_REQUIRE(ledger.has_value());
  FEL_EXPECT_EQ(ledger.value()->as_of(), far_future);
  auto periods = ledger.value()->periods();
  FEL_REQUIRE(periods.size() == 1);
  auto closed = ledger.value()->close_period(
      CloseRequest{periods.front().id, far_future, "close", "test", false});
  FEL_REQUIRE(closed.has_value());
  FEL_EXPECT_EQ(closed.value().claims.size(), std::size_t{0});
  FEL_EXPECT_EQ(closed.value().evidence_admitted, std::uint64_t{0});
  bool saw_expired = false;
  for (const auto& unknown : closed.value().unknowns) {
    if (unknown.reason == UnknownReason::ExpiredEvidence) {
      saw_expired = true;
    }
  }
  FEL_EXPECT(saw_expired);
}

FEL_TEST(integration, corrupt_manifest_is_rebuilt_from_the_segments) {
  StoreFixture env;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
    FEL_REQUIRE(period.has_value());
    FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 700)})));
    FEL_ASSERT_OK(ledger.value()->flush());
  }
  const std::string manifest = (std::filesystem::path(env.directory.child("store")) /
                                "fel.manifest.json")
                                   .string();
  {
    std::FILE* file = std::fopen(manifest.c_str(), "wb");
    FEL_REQUIRE(file != nullptr);
    const std::string garbage = "{\"format_version\": 1, \"integrity\": \"deadbeef\"}";
    std::fwrite(garbage.data(), 1, garbage.size(), file);
    std::fclose(file);
  }
  auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
  FEL_REQUIRE(ledger.has_value());
  FEL_EXPECT(ledger.value()->store_recovery().manifest_rebuilt);
  FEL_EXPECT_EQ(ledger.value()->retained_observation_count(), std::uint64_t{1});
}

FEL_TEST(integration, strict_recovery_refuses_a_corrupt_manifest) {
  StoreFixture env;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
    FEL_REQUIRE(period.has_value());
    FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 700)})));
  }
  const std::string manifest =
      (std::filesystem::path(env.directory.child("store")) / "fel.manifest.json").string();
  {
    std::FILE* file = std::fopen(manifest.c_str(), "wb");
    FEL_REQUIRE(file != nullptr);
    const std::string garbage = "not json at all";
    std::fwrite(garbage.data(), 1, garbage.size(), file);
    std::fclose(file);
  }
  auto policy = feltest::test_policy();
  policy.recovery = RecoveryPolicy::Strict;
  auto ledger = Ledger::open(options_for(env, env.fixture.period_end, policy));
  FEL_EXPECT(!ledger.has_value());
}

FEL_TEST(integration, a_torn_tail_is_discarded_and_the_prefix_is_kept) {
  StoreFixture env;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
    FEL_REQUIRE(ledger.has_value());
    auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
    FEL_REQUIRE(period.has_value());
    FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 100),
                                                              sample(env.fixture, 2, 200)})));
    FEL_ASSERT_OK(ledger.value()->flush());
  }
  const auto store = std::filesystem::path(env.directory.child("store"));
  std::string segment;
  for (const auto& entry : std::filesystem::directory_iterator(store)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("seg-", 0) == 0) {
      segment = entry.path().string();
      break;
    }
  }
  FEL_REQUIRE(!segment.empty());
  // Simulate a crash that lost the tail of the last write: the segment ends in
  // the middle of its footer instead of after it.
  const auto original_size = std::filesystem::file_size(segment);
  FEL_REQUIRE(original_size > 20);
  std::filesystem::resize_file(segment, original_size - 20);

  auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
  FEL_REQUIRE(ledger.has_value());
  const RecoveryReport& recovery = ledger.value()->store_recovery();
  FEL_EXPECT(recovery.tail_bytes_discarded > 0);
  FEL_EXPECT(!recovery.clean());
  FEL_EXPECT_EQ(ledger.value()->retained_observation_count(), std::uint64_t{2});
}

FEL_TEST(integration, integrity_check_reports_a_healthy_store) {
  StoreFixture env;
  auto ledger = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
  FEL_REQUIRE(ledger.has_value());
  auto period = ledger.value()->open_period(env.fixture.period_start, env.fixture.period_end, "p");
  FEL_REQUIRE(period.has_value());
  FEL_ASSERT_OK(ledger.value()->ingest(feltest::make_batch({sample(env.fixture, 1, 100)})));
  FEL_ASSERT_OK(ledger.value()->close_period(
      CloseRequest{period.value(), env.fixture.period_end, "close", "test", false}));
  auto integrity = ledger.value()->verify_integrity();
  FEL_REQUIRE(integrity.has_value());
  FEL_EXPECT(integrity.value().ok);
  FEL_EXPECT_EQ(integrity.value().revisions_failed, std::uint64_t{0});
  FEL_EXPECT_EQ(integrity.value().lineage_breaks, std::uint64_t{0});
  FEL_EXPECT_EQ(integrity.value().lock_audit_reentrancy, std::uint64_t{0});
  FEL_EXPECT_EQ(integrity.value().lock_audit_order, std::uint64_t{0});
}

FEL_TEST(integration, a_second_writer_is_refused_while_the_store_is_locked) {
  StoreFixture env;
  auto first = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
  FEL_REQUIRE(first.has_value());
  auto second = Ledger::open(options_for(env, env.fixture.period_end, feltest::test_policy()));
  FEL_EXPECT(!second.has_value());
  if (!second.has_value()) {
    FEL_EXPECT_EQ(second.error().code, ErrorCode::StoreLocked);
  }
}

FEL_TEST(integration, a_differing_policy_is_refused_on_reopen) {
  StoreFixture env;
  auto policy = feltest::test_policy();
  policy.conflict = ConflictPolicy::StrictFail;
  {
    auto ledger = Ledger::open(options_for(env, env.fixture.period_end, policy));
    FEL_REQUIRE(ledger.has_value());
  }
  auto other = feltest::test_policy();
  other.residual = ResidualPolicy::StrictReject;
  auto ledger = Ledger::open(options_for(env, env.fixture.period_end, other));
  FEL_EXPECT(!ledger.has_value());
  if (!ledger.has_value()) {
    FEL_EXPECT_EQ(ledger.error().code, ErrorCode::PolicyRevisionMismatch);
  }
}
