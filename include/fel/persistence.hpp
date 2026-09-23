// Fabric Efficiency Ledger - versioned, integrity checked persistence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/crypto.hpp"
#include "fel/id.hpp"
#include "fel/policy.hpp"
#include "fel/time.hpp"

namespace fel {

enum class StoreOpenMode : std::uint8_t {
  CreateNew = 1,  // fails if the store already exists
  OpenExisting = 2,
  OpenOrCreate = 3,
};

[[nodiscard]] std::string_view store_open_mode_name(StoreOpenMode mode) noexcept;

struct StoreConfig {
  std::string path;
  StoreOpenMode mode = StoreOpenMode::OpenOrCreate;
  RecoveryPolicy recovery = RecoveryPolicy::Conservative;
  std::uint64_t max_bytes = limits::kMaxStoreBytes;
  std::uint64_t max_segment_bytes = limits::kMaxSegmentBytes;
  std::uint64_t max_segments = limits::kMaxSegments;
  bool fsync_on_commit = true;
  bool take_exclusive_lock = true;
  // How long to wait for the store lock before failing with StoreLocked.
  std::uint32_t lock_timeout_ms = 0;
  // Bounded keep-alive window for lock contention diagnostics. Never unbounded.
  std::uint32_t lock_hold_max_ms = 30000;
};

struct RecoveryReport {
  bool manifest_present = false;
  bool manifest_valid = false;
  bool manifest_rebuilt = false;
  std::uint64_t segments_present = 0;
  std::uint64_t segments_scanned = 0;
  std::uint64_t segments_damaged = 0;
  std::uint64_t segments_missing = 0;
  std::uint64_t segments_orphaned = 0;
  std::uint64_t records_recovered = 0;
  std::uint64_t records_discarded = 0;
  std::uint64_t checksum_failures = 0;
  std::uint64_t tail_bytes_discarded = 0;
  std::uint64_t bytes_recovered = 0;
  TimePoint watermark;
  bool conservative = true;
  bool truncated_at_damage = false;
  std::vector<std::string> diagnostics;

  [[nodiscard]] bool clean() const noexcept {
    return segments_damaged == 0 && records_discarded == 0 && checksum_failures == 0 &&
           tail_bytes_discarded == 0 && segments_missing == 0;
  }
  [[nodiscard]] std::string canonical_form() const;
};

// The kind of record stored in a segment. Records are self describing and
// forward compatible: an unknown kind is retained but ignored by replay.
enum class RecordKind : std::uint8_t {
  PolicySnapshot = 1,
  TopologySnapshot = 2,
  Observation = 3,
  PeriodOpened = 4,
  PeriodClosed = 5,
  Correction = 6,
  Compaction = 7,
  Watermark = 8,
};

[[nodiscard]] std::string_view record_kind_name(RecordKind kind) noexcept;
[[nodiscard]] std::optional<RecordKind> record_kind_from_name(std::string_view name) noexcept;

struct StoreRecord {
  RecordKind kind = RecordKind::Observation;
  std::uint64_t ordinal = 0;
  TimePoint written_at;
  std::string payload;
};

struct SegmentInfo {
  std::string file;
  std::uint64_t segment_id = 0;
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  Digest256 digest;
  bool damaged = false;
};

struct StoreStats {
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t segments = 0;
  std::uint64_t revision = 0;
  std::uint64_t observations_retained = 0;
  TimePoint watermark;
  TimePoint first_write;
  bool read_only = false;
  bool locked = false;
};

struct CompactionReport {
  bool performed = false;
  std::uint64_t segments_before = 0;
  std::uint64_t segments_after = 0;
  std::uint64_t bytes_before = 0;
  std::uint64_t bytes_after = 0;
  std::uint64_t records_dropped = 0;
  std::uint64_t periods_folded = 0;
  std::vector<std::string> diagnostics;
};

// A crash safe, append only, integrity checked record store.
//
// Layout on disk:
//   fel.manifest.json   atomic manifest: store id, format version, segments
//   fel.lock            advisory exclusive lock held for the process lifetime
//   seg-<id>.felseg     length + CRC framed records with a digest footer
//
// Recovery never rewrites history silently: damage is reported, the longest
// valid prefix is used, and everything after the first invalid frame is
// discarded and surfaced in the RecoveryReport.
class LedgerStore {
 public:
  using RecordVisitor = std::function<void(const StoreRecord&)>;

  LedgerStore(const LedgerStore&) = delete;
  LedgerStore& operator=(const LedgerStore&) = delete;
  ~LedgerStore();

  [[nodiscard]] static Result<std::unique_ptr<LedgerStore>> open(const StoreConfig& config);

  [[nodiscard]] Result<void> append(RecordKind kind, std::string payload, TimePoint written_at);
  [[nodiscard]] Result<void> append_many(const std::vector<std::pair<RecordKind, std::string>>& records,
                                         TimePoint written_at);
  [[nodiscard]] Result<void> flush();

  // Sequential scan of every recoverable record, in write order.
  [[nodiscard]] Result<std::uint64_t> replay(const RecordVisitor& visitor) const;

  [[nodiscard]] Result<CompactionReport> compact(
      const std::function<bool(const StoreRecord&)>& keep, TimePoint now);

  [[nodiscard]] const StoreId& store_id() const noexcept { return store_id_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] StoreStats stats() const;
  [[nodiscard]] const std::string& path() const noexcept { return config_.path; }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }
  [[nodiscard]] TimePoint watermark() const noexcept { return watermark_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] const std::vector<SegmentInfo>& segments() const noexcept { return segments_; }

  // Verifies every segment digest and frame checksum without mutating anything.
  [[nodiscard]] Result<RecoveryReport> verify() const;

 private:
  explicit LedgerStore(StoreConfig config) : config_(std::move(config)) {}

  [[nodiscard]] Result<void> load_manifest();
  [[nodiscard]] Result<void> rebuild_manifest_from_segments();
  [[nodiscard]] Result<void> write_manifest();
  [[nodiscard]] Result<void> scan_segment(const std::string& file, SegmentInfo* info,
                                          const RecordVisitor* visitor) const;
  void recompute_totals() noexcept;
  [[nodiscard]] Result<void> rotate_if_needed(std::uint64_t incoming_bytes);
  [[nodiscard]] std::string segment_path(std::uint64_t segment_id) const;
  [[nodiscard]] Result<void> acquire_lock();
  void release_lock();
  [[nodiscard]] Result<void> open_segment(std::uint64_t segment_id, TimePoint created_at);
  [[nodiscard]] Result<void> write_frame(RecordKind kind, std::string_view payload,
                                         TimePoint written_at);
  [[nodiscard]] Result<void> seal_segment();
  [[nodiscard]] Result<void> sweep_orphans();

  StoreConfig config_;
  StoreId store_id_;
  mutable RecoveryReport recovery_{};
  std::vector<SegmentInfo> segments_;
  std::uint64_t next_segment_id_ = 1;
  std::uint64_t current_segment_bytes_ = 0;
  std::uint64_t current_segment_records_ = 0;
  std::uint64_t total_records_ = 0;
  std::uint64_t total_bytes_ = 0;
  std::uint64_t revision_ = 0;
  TimePoint watermark_;
  TimePoint first_write_;
  bool read_only_ = false;
  bool locked_ = false;
  bool manifest_dirty_ = false;
  std::FILE* segment_file_ = nullptr;
  void* lock_handle_ = nullptr;
  std::uint64_t current_segment_id_ = 0;
  Sha256 segment_hasher_;
};

}  // namespace fel
