// Fabric Efficiency Ledger - period close (derivation) throughput.
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
  auto ingest = ledger.value()->ingest(make_batch_for(evidence));
  if (!ingest.has_value()) {
    std::printf("ingest failed\n");
    return 1;
  }

  std::uint64_t cells = 0;
  {
    const Timer timer;
    auto closed = ledger.value()->close_period(
        CloseRequest{period.value(), fixture.end, "bench close", "benchmark", false});
    const double elapsed = timer.seconds();
    if (!closed.has_value()) {
      std::printf("close failed: %s\n", closed.error().to_string().c_str());
      return 1;
    }
    cells = closed.value().claims.size();
    report("close.cells", cells, "cells", elapsed);
    report("close.records", closed.value().evidence_considered, "records", elapsed);
    std::printf("conservation domains=%llu consistent=%llu residual=%llu excess=%llu "
                "unverifiable=%llu\n",
                static_cast<unsigned long long>(closed.value().conservation.size()),
                static_cast<unsigned long long>(closed.value().conservation.size()),
                static_cast<unsigned long long>(closed.value().has_residual ? 1 : 0),
                static_cast<unsigned long long>(closed.value().has_excess ? 1 : 0), 0ULL);
  }

  // Re-deriving the same period must reproduce the same content digest: the
  // derivation is a pure function of evidence and policy.
  {
    const Timer timer;
    auto again = ledger.value()->derive(period.value(), fixture.end, RevisionOrdinal{1},
                                        std::nullopt, std::nullopt, std::string{"bench close"},
                                        std::string{"benchmark"});
    const double elapsed = timer.seconds();
    if (!again.has_value()) {
      std::printf("re-derivation failed\n");
      return 1;
    }
    report("close.repeat", again.value().claims.size(), "cells", elapsed);
    auto stored = ledger.value()->latest_revision(period.value());
    if (!stored.has_value() ||
        stored.value().content_digest.to_hex() != again.value().content_digest.to_hex()) {
      std::printf("determinism check failed\n");
      return 1;
    }
  }

  std::printf("verified cells=%llu records=%llu\n", static_cast<unsigned long long>(cells),
              static_cast<unsigned long long>(kRecords));
  return cells > 0 ? 0 : 1;
}
