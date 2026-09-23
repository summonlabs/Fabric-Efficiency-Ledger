// Fabric Efficiency Ledger - query, rollup and aggregation throughput.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>

#include "bench_support.hpp"

int main() {
  using namespace bench;
  constexpr std::size_t kRecords = 100000;
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
  if (!ledger.value()->ingest(make_batch_for(evidence)).has_value()) {
    std::printf("ingest failed\n");
    return 1;
  }
  auto closed = ledger.value()->close_period(
      CloseRequest{period.value(), fixture.end, "bench close", "benchmark", false});
  if (!closed.has_value()) {
    std::printf("close failed\n");
    return 1;
  }

  constexpr int kIterations = 50;
  std::uint64_t rows = 0;
  {
    const Timer timer;
    for (int i = 0; i < kIterations; ++i) {
      QueryRequest request;
      request.period = period.value();
      auto summary = ledger.value()->query(request);
      if (!summary.has_value()) {
        std::printf("query failed\n");
        return 1;
      }
      rows += summary.value().rows.size() + summary.value().rolled_up.size();
    }
    report("query.rows", rows, "rows", timer.seconds());
  }

  std::uint64_t buckets = 0;
  {
    const Timer timer;
    for (int i = 0; i < kIterations; ++i) {
      AggregateRequest request;
      request.period = period.value();
      request.axis = AggregateAxis::Category;
      auto result = ledger.value()->aggregate(request);
      if (!result.has_value()) {
        std::printf("aggregate failed\n");
        return 1;
      }
      buckets += result.value().buckets.size();
    }
    report("aggregate.buckets", buckets, "buckets", timer.seconds());
  }

  std::uint64_t exported = 0;
  {
    const Timer timer;
    for (int i = 0; i < 5; ++i) {
      ExportRequest request;
      request.period = period.value();
      request.format = ExportFormat::JsonLines;
      auto result = ledger.value()->export_period(request);
      if (!result.has_value()) {
        std::printf("export failed\n");
        return 1;
      }
      exported += result.value().rows;
    }
    report("export.rows", exported, "rows", timer.seconds());
  }

  std::uint64_t explained = 0;
  {
    const Timer timer;
    for (const auto& claim : closed.value().claims) {
      ExplainRequest request;
      request.period = period.value();
      request.identity = claim.identity;
      auto explanation = ledger.value()->explain(request);
      if (!explanation.has_value()) {
        std::printf("explain failed\n");
        return 1;
      }
      ++explained;
    }
    report("explain.cells", explained, "cells", timer.seconds());
  }

  std::printf("verified rows=%llu buckets=%llu exported=%llu explained=%llu\n",
              static_cast<unsigned long long>(rows), static_cast<unsigned long long>(buckets),
              static_cast<unsigned long long>(exported),
              static_cast<unsigned long long>(explained));
  return rows > 0 && buckets > 0 ? 0 : 1;
}
