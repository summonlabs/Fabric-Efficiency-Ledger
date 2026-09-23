// Fabric Efficiency Ledger - durability throughput and recovery cost.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>

#include "bench_support.hpp"
#include "fel/persistence.hpp"

namespace {

[[nodiscard]] std::string scratch_directory() {
  const auto base = std::filesystem::temp_directory_path();
  const std::string path = (base / "fel-bench-persist").string();
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

}  // namespace

int main() {
  using namespace bench;
  constexpr std::size_t kRecords = 50000;
  const std::string root = scratch_directory();

  fel::StoreConfig config;
  config.path = root;
  config.mode = fel::StoreOpenMode::OpenOrCreate;
  config.fsync_on_commit = true;

  std::uint64_t written = 0;
  std::uint64_t bytes = 0;
  {
    auto store = LedgerStore::open(config);
    if (!store.has_value()) {
      std::printf("store open failed: %s\n", store.error().to_string().c_str());
      return 1;
    }
    const Timer timer;
    const TimePoint when = TimePoint::from_seconds(1700000000);
    // Records are committed in batches, which is how the ingest path uses the
    // store: one durable commit per batch, one segment per commit.
    constexpr std::size_t kBatch = 1000;
    std::vector<std::pair<RecordKind, std::string>> batch;
    batch.reserve(kBatch);
    for (std::size_t i = 0; i < kRecords; ++i) {
      batch.emplace_back(RecordKind::Observation, "{\"record\":" + std::to_string(i) + "}");
      if (batch.size() == kBatch || i + 1 == kRecords) {
        auto appended = store.value()->append_many(batch, when);
        if (!appended.has_value()) {
          std::printf("append failed: %s\n", appended.error().to_string().c_str());
          return 1;
        }
        written += batch.size();
        batch.clear();
      }
    }
    bytes = store.value()->stats().bytes;
    report("persist.committed", written, "records", timer.seconds());
  }

  // Recovering the same store must read back every committed record.
  {
    auto store = LedgerStore::open(config);
    if (!store.has_value()) {
      std::printf("reopen failed: %s\n", store.error().to_string().c_str());
      return 1;
    }
    std::uint64_t replayed = 0;
    const Timer timer;
    auto outcome = store.value()->replay([&replayed](const StoreRecord&) { ++replayed; });
    if (!outcome.has_value()) {
      std::printf("replay failed\n");
      return 1;
    }
    report("persist.recovered", replayed, "records", timer.seconds());
    std::printf("store bytes=%llu segments=%llu clean=%s\n",
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(store.value()->stats().segments),
                store.value()->recovery().clean() ? "true" : "false");
    if (replayed != kRecords) {
      std::printf("recovery mismatch\n");
      return 1;
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::printf("verified written=%llu recovered=%llu\n", static_cast<unsigned long long>(written),
              static_cast<unsigned long long>(kRecords));
  return written == kRecords ? 0 : 1;
}
