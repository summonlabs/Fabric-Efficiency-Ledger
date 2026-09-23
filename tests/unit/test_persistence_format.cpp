// Fabric Efficiency Ledger - store framing, integrity and recovery semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fel/persistence.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] StoreConfig config_for(const std::string& path) {
  StoreConfig config;
  config.path = path;
  config.mode = StoreOpenMode::OpenOrCreate;
  config.fsync_on_commit = true;
  return config;
}

void overwrite(const std::string& path, const std::string& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file != nullptr) {
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
  }
}

}  // namespace

FEL_TEST(unit, store_appends_and_replays_in_write_order) {
  feltest::ScopedTempDir directory("store-format");
  auto store = LedgerStore::open(config_for(directory.child("store")));
  FEL_REQUIRE(store.has_value());
  const TimePoint when = TimePoint::from_seconds(1700000000);
  FEL_ASSERT_OK(store.value()->append(RecordKind::PolicySnapshot, "{\"a\":1}", when));
  FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, "{\"b\":2}", when));
  FEL_ASSERT_OK(store.value()->append(RecordKind::PeriodOpened, "{\"c\":3}", when));
  FEL_EXPECT_EQ(store.value()->stats().records, std::uint64_t{3});
  FEL_EXPECT(store.value()->stats().bytes > 0);

  std::vector<RecordKind> kinds;
  std::vector<std::string> payloads;
  FEL_ASSERT_OK(store.value()->replay([&](const StoreRecord& record) {
    kinds.push_back(record.kind);
    payloads.push_back(record.payload);
  }));
  FEL_REQUIRE(kinds.size() == 3);
  FEL_EXPECT_EQ(kinds[0], RecordKind::PolicySnapshot);
  FEL_EXPECT_EQ(kinds[1], RecordKind::Observation);
  FEL_EXPECT_EQ(kinds[2], RecordKind::PeriodOpened);
  FEL_EXPECT_EQ(payloads[1], std::string("{\"b\":2}"));
  FEL_EXPECT_EQ(store.value()->watermark(), when);
}

FEL_TEST(unit, manifest_is_integrity_checked) {
  feltest::ScopedTempDir directory("store-manifest");
  const std::string path = directory.child("store");
  {
    auto store = LedgerStore::open(config_for(path));
    FEL_REQUIRE(store.has_value());
    FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, "payload",
                                        TimePoint::from_seconds(1)));
  }
  const auto manifest = std::filesystem::path(path) / "fel.manifest.json";
  FEL_REQUIRE(std::filesystem::exists(manifest));
  const std::string body = "{\"format_version\":1,\"integrity\":\"00\"}";
  overwrite(manifest.string(), body);

  auto store = LedgerStore::open(config_for(path));
  FEL_REQUIRE(store.has_value());
  FEL_EXPECT(store.value()->recovery().manifest_rebuilt);
  FEL_EXPECT_EQ(store.value()->stats().records, std::uint64_t{1});
}

FEL_TEST(unit, strict_recovery_refuses_a_corrupt_manifest) {
  feltest::ScopedTempDir directory("store-strict");
  const std::string path = directory.child("store");
  {
    auto store = LedgerStore::open(config_for(path));
    FEL_REQUIRE(store.has_value());
    FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, "payload",
                                        TimePoint::from_seconds(1)));
  }
  overwrite((std::filesystem::path(path) / "fel.manifest.json").string(), "garbage");
  auto config = config_for(path);
  config.recovery = RecoveryPolicy::Strict;
  auto store = LedgerStore::open(config);
  FEL_EXPECT(!store.has_value());
  if (!store.has_value()) {
    FEL_EXPECT_EQ(store.error().code, ErrorCode::ManifestCorrupt);
  }
}

FEL_TEST(unit, payload_and_capacity_budgets_are_enforced) {
  feltest::ScopedTempDir directory("store-budgets");
  const std::string path = directory.child("store");
  // Each 2048 byte payload occupies 32 (header) + 2048 + 17 (record header)
  // + 8 (frame header) + 52 (footer) = 2157 bytes, because a segment is sealed
  // once per commit. A 4600 byte budget therefore admits exactly two commits.
  auto config = config_for(path);
  config.max_bytes = 4600;
  auto store = LedgerStore::open(config);
  FEL_REQUIRE(store.has_value());
  const std::string oversized(limits::kMaxRecordBytes + 1, 'x');
  FEL_ASSERT_ERR(store.value()->append(RecordKind::Observation, oversized,
                                       TimePoint::from_seconds(1)),
                 ErrorCode::PayloadTooLarge);

  const std::string large(2048, 'y');
  FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, large, TimePoint::from_seconds(1)));
  FEL_EXPECT_EQ(store.value()->stats().bytes, std::uint64_t{2157});
  FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, large, TimePoint::from_seconds(1)));
  FEL_EXPECT_EQ(store.value()->stats().bytes, std::uint64_t{4314});
  FEL_ASSERT_ERR(store.value()->append(RecordKind::Observation, large,
                                       TimePoint::from_seconds(1)),
                 ErrorCode::CapacityExceeded);
}

FEL_TEST(unit, exclusive_lock_excludes_a_second_holder_in_process) {
  feltest::ScopedTempDir directory("store-lock");
  const std::string path = directory.child("store");
  auto first = LedgerStore::open(config_for(path));
  FEL_REQUIRE(first.has_value());
  auto second = LedgerStore::open(config_for(path));
  FEL_EXPECT(!second.has_value());
  if (!second.has_value()) {
    FEL_EXPECT_EQ(second.error().code, ErrorCode::StoreLocked);
  }
}

FEL_TEST(unit, opening_without_the_lock_yields_a_read_only_store) {
  feltest::ScopedTempDir directory("store-readonly");
  const std::string path = directory.child("store");
  auto config = config_for(path);
  config.take_exclusive_lock = false;
  auto store = LedgerStore::open(config);
  FEL_REQUIRE(store.has_value());
  FEL_REQUIRE(store.value()->read_only());
  FEL_ASSERT_ERR(store.value()->append(RecordKind::Observation, "x", TimePoint::from_seconds(1)),
                 ErrorCode::StoreReadOnly);
}

FEL_TEST(unit, verify_detects_a_tampered_segment) {
  feltest::ScopedTempDir directory("store-verify");
  const std::string path = directory.child("store");
  {
    auto store = LedgerStore::open(config_for(path));
    FEL_REQUIRE(store.has_value());
    FEL_ASSERT_OK(store.value()->append(RecordKind::Observation, "original",
                                        TimePoint::from_seconds(1)));
  }
  std::string segment;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (entry.path().filename().string().rfind("seg-", 0) == 0) {
      segment = entry.path().string();
    }
  }
  FEL_REQUIRE(!segment.empty());
  // Flip a byte in the middle of the record payload.
  std::FILE* file = std::fopen(segment.c_str(), "r+b");
  FEL_REQUIRE(file != nullptr);
  std::fseek(file, 40, SEEK_SET);
  const int byte = std::fgetc(file);
  std::fseek(file, 40, SEEK_SET);
  std::fputc(byte ^ 0xFF, file);
  std::fclose(file);

  auto store = LedgerStore::open(config_for(path));
  FEL_REQUIRE(store.has_value());
  auto report = store.value()->verify();
  FEL_REQUIRE(report.has_value());
  FEL_EXPECT(!report.value().clean());
  FEL_EXPECT(report.value().segments_damaged > 0 || report.value().checksum_failures > 0);
}

FEL_TEST(unit, record_kind_names_round_trip) {
  for (std::size_t i = 1; i <= 8; ++i) {
    const auto kind = static_cast<RecordKind>(i);
    const auto parsed = record_kind_from_name(record_kind_name(kind));
    FEL_REQUIRE(parsed.has_value());
    FEL_EXPECT_EQ(parsed.value(), kind);
  }
  FEL_EXPECT(!record_kind_from_name("unknown-kind").has_value());
  FEL_EXPECT_EQ(std::string(store_open_mode_name(StoreOpenMode::CreateNew)),
                std::string("create-new"));
}
