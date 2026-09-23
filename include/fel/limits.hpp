// Fabric Efficiency Ledger - hard resource bounds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fel {

// "Bound workers, queues, payloads, metadata, history, result sets, persistence
// growth, and aggregation windows." Every one of these is enforced at the point
// of use and is covered by the policy revision digest.
namespace limits {

// Payload / metadata
inline constexpr std::size_t kMaxRecordBytes = 1U << 20;  // 1 MiB single evidence record
inline constexpr std::size_t kMaxBatchRecords = 200000;   // records per ingest batch
inline constexpr std::size_t kMaxMetadataEntries = 32;    // key/value pairs per record
inline constexpr std::size_t kMaxMetadataKeyBytes = 64;
inline constexpr std::size_t kMaxMetadataValueBytes = 512;
inline constexpr std::size_t kMaxLabelBytes = 256;
inline constexpr std::size_t kMaxJsonDepth = 32;
inline constexpr std::size_t kMaxJsonTokens = 4000000;

// Topology / policy documents
inline constexpr std::size_t kMaxScopes = 1000000;
inline constexpr std::size_t kMaxResources = 4000000;
inline constexpr std::size_t kMaxFlows = 4000000;
inline constexpr std::size_t kMaxPaths = 1000000;
inline constexpr std::size_t kMaxReservations = 1000000;
inline constexpr std::size_t kMaxSources = 4096;
inline constexpr std::size_t kMaxGenerations = 4096;
inline constexpr std::size_t kMaxIncarnationsPerSource = 4096;
inline constexpr std::size_t kMaxPeriods = 100000;
inline constexpr std::size_t kMaxCorrectionsPerPeriod = 256;

// Runtime
inline constexpr std::uint32_t kMaxWorkers = 64;
inline constexpr std::size_t kMaxQueueDepth = 65536;

// Results
inline constexpr std::size_t kMaxResultRows = 100000;
inline constexpr std::size_t kMaxExplanationEntries = 20000;
inline constexpr std::size_t kMaxConflictRecords = 20000;

// Aggregation
inline constexpr std::size_t kMaxAggregationBuckets = 100000;
inline constexpr std::int64_t kMinWindowNanos = 1000000;  // 1 ms
inline constexpr std::int64_t kMaxWindowNanos = 366LL * 24 * 3600 * 1000000000LL;  // 366 days

// Persistence
inline constexpr std::uint64_t kMaxSegmentBytes = 256ULL * 1024 * 1024;  // 256 MiB
inline constexpr std::uint64_t kMaxStoreBytes = 64ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxSegments = 4096;
inline constexpr std::uint64_t kMaxRetainedObservations = 20000000ULL;

}  // namespace limits

}  // namespace fel
