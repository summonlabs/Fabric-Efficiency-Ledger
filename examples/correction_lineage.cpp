// Fabric Efficiency Ledger - immutable closes and correction lineage.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <string>
#include <vector>

#include "example_support.hpp"

using namespace fel;

int main() {
  const auto env = example::environment();
  auto ledger = Ledger::in_memory(example::policy(), env.topology, env.period_end);
  if (!ledger.has_value()) {
    std::printf("ledger open failed\n");
    return 1;
  }
  auto period = ledger.value()->open_period(env.period_start, env.period_end, "hour-1");
  if (!period.has_value()) {
    std::printf("period open failed\n");
    return 1;
  }

  example::ObservationBatchBuilder builder(env);
  builder.add(1, 1000, Category::UsefulDeliveredWork, 0);
  if (!ledger.value()->ingest(builder.batch()).has_value()) {
    std::printf("ingest failed\n");
    return 1;
  }
  auto first = ledger.value()->close_period(
      CloseRequest{period.value(), env.period_end, "close", "example", false});
  if (!first.has_value()) {
    std::printf("close failed\n");
    return 1;
  }
  std::printf("revision 1 digest %s\n", first.value().content_digest.to_hex().c_str());

  // Evidence that arrives after the close is held back until an operator asks
  // for a correction; the original revision is never rewritten.
  builder.add(2, 500, Category::UsefulDeliveredWork, 20);
  if (!ledger.value()->ingest(builder.batch()).has_value()) {
    std::printf("late ingest failed\n");
    return 1;
  }
  auto corrected = ledger.value()->correct_period(CorrectionRequest{
      period.value(), env.period_end + Duration::from_hours(1), "late evidence", "example", false});
  if (!corrected.has_value()) {
    std::printf("correction failed\n");
    return 1;
  }
  std::printf("revision %llu digest %s parent %s\n",
              static_cast<unsigned long long>(corrected.value().revision.value()),
              corrected.value().content_digest.to_hex().c_str(),
              corrected.value().parent_digest->to_hex().c_str());

  for (const auto& revision : ledger.value()->revisions_of(period.value())) {
    std::uint64_t total = 0;
    for (const auto& claim : revision.claims) {
      total += claim.cell.known_total;
    }
    std::printf("  revision %llu total=%llu\n",
                static_cast<unsigned long long>(revision.revision.value()),
                static_cast<unsigned long long>(total));
  }
  auto integrity = ledger.value()->verify_integrity();
  if (integrity.has_value()) {
    std::printf("integrity ok=%s lineage_breaks=%llu\n", integrity.value().ok ? "true" : "false",
                static_cast<unsigned long long>(integrity.value().lineage_breaks));
  }
  return 0;
}
