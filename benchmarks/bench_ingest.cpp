// Fabric Efficiency Ledger - evidence ingest throughput.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>

#include "bench_support.hpp"

int main() {
  using namespace bench;
  constexpr std::size_t kRecords = 200000;
  const auto fixture =
      make_fixture(64, TimePoint::from_seconds(1700000000), Duration::from_hours(24));
  const auto evidence = make_evidence(fixture, kRecords, 24 * 12);

  auto ledger = Ledger::in_memory(bench_policy(), fixture.topology, fixture.end);
  if (!ledger.has_value()) {
    std::printf("ledger open failed\n");
    return 1;
  }
  auto period = ledger.value()->open_period(fixture.start, fixture.end, "bench");
  if (!period.has_value()) {
    std::printf("period open failed\n");
    return 1;
  }

  std::uint64_t accepted = 0;
  {
    const Timer timer;
    auto report = ledger.value()->ingest(make_batch_for(evidence));
    const double elapsed = timer.seconds();
    if (!report.has_value()) {
      std::printf("ingest failed: %s\n", report.error().to_string().c_str());
      return 1;
    }
    accepted = report.value().accepted;
    bench::report("ingest.sequential", accepted, "records", elapsed);
  }

  // Re-ingesting the identical batch must be free of accounting work: every
  // record is recognised as a duplicate.
  {
    const Timer timer;
    auto report = ledger.value()->ingest(make_batch_for(evidence));
    const double elapsed = timer.seconds();
    if (!report.has_value()) {
      std::printf("duplicate ingest failed\n");
      return 1;
    }
    bench::report("ingest.duplicates", report.value().duplicate_suppressed, "records", elapsed);
    if (report.value().accepted != 0) {
      std::printf("duplicate suppression failed\n");
      return 1;
    }
  }

  const std::uint64_t retained = ledger.value()->retained_observation_count();
  std::printf("verified accepted=%llu retained=%llu of %llu\n",
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(retained),
              static_cast<unsigned long long>(kRecords));
  return accepted == kRecords && retained == kRecords ? 0 : 1;
}
