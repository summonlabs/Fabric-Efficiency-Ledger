// Fabric Efficiency Ledger - versioned, integrity checked persistence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "fel/checked.hpp"
#include "fel/serialize.hpp"
#include "fel/version.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fel {
namespace {

constexpr const char kSegmentMagic[8] = {'F', 'E', 'L', 'S', 'E', 'G', '0', '1'};
constexpr const char kFooterMagic[8] = {'F', 'E', 'L', 'F', 'O', 'O', 'T', '1'};
constexpr std::uint32_t kSegmentHeaderBytes = 32;
constexpr std::uint32_t kFrameHeaderBytes = 8;
constexpr std::uint32_t kFooterBytes = 8 + 8 + 32 + 4;
constexpr std::uint32_t kRecordHeaderBytes = 1 + 8 + 8;
constexpr const char* kManifestName = "fel.manifest.json";
constexpr const char* kLockName = "fel.lock";
constexpr const char* kSegmentPrefix = "seg-";
constexpr const char* kSegmentSuffix = ".felseg";

constexpr const char* kRecordKindNames[] = {"policy-snapshot", "topology-snapshot", "observation",
                                            "period-opened",   "period-closed",     "correction",
                                            "compaction",      "watermark"};

void put_u32(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFFU);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
  out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
  return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
         (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | static_cast<std::uint64_t>(in[static_cast<std::size_t>(i)]);
  }
  return value;
}

[[nodiscard]] bool read_exact(std::FILE* file, void* buffer, std::size_t bytes) {
  return std::fread(buffer, 1, bytes, file) == bytes;
}

[[nodiscard]] bool write_exact(std::FILE* file, const void* buffer, std::size_t bytes) {
  return std::fwrite(buffer, 1, bytes, file) == bytes;
}

void sync_file(std::FILE* file) {
  if (file == nullptr) {
    return;
  }
  std::fflush(file);
#ifdef _WIN32
  _commit(_fileno(file));
#else
  ::fsync(::fileno(file));
#endif
}

[[nodiscard]] std::uint64_t file_size_of(std::FILE* file) {
  if (file == nullptr) {
    return 0;
  }
  const long current = std::ftell(file);
  if (current < 0) {
    return 0;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return 0;
  }
  const long end = std::ftell(file);
  std::fseek(file, current, SEEK_SET);
  return end < 0 ? 0 : static_cast<std::uint64_t>(end);
}

}  // namespace

std::string_view store_open_mode_name(StoreOpenMode mode) noexcept {
  switch (mode) {
    case StoreOpenMode::CreateNew:
      return "create-new";
    case StoreOpenMode::OpenExisting:
      return "open-existing";
    case StoreOpenMode::OpenOrCreate:
      return "open-or-create";
  }
  return "invalid";
}

std::string_view record_kind_name(RecordKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > 8) {
    return "invalid";
  }
  return kRecordKindNames[index - 1];
}

std::optional<RecordKind> record_kind_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    if (name == kRecordKindNames[i]) {
      return static_cast<RecordKind>(i + 1);
    }
  }
  return std::nullopt;
}

std::string RecoveryReport::canonical_form() const {
  FieldWriter writer;
  writer.field("recovery-report/1");
  writer.field_bool(manifest_present);
  writer.field_bool(manifest_valid);
  writer.field_bool(manifest_rebuilt);
  writer.field_u64(segments_present);
  writer.field_u64(segments_scanned);
  writer.field_u64(segments_damaged);
  writer.field_u64(segments_missing);
  writer.field_u64(segments_orphaned);
  writer.field_u64(records_recovered);
  writer.field_u64(records_discarded);
  writer.field_u64(checksum_failures);
  writer.field_u64(tail_bytes_discarded);
  writer.field_u64(bytes_recovered);
  writer.field_i64(watermark.nanos());
  writer.field_bool(conservative);
  writer.field_bool(truncated_at_damage);
  for (const auto& line : diagnostics) {
    writer.field(line);
  }
  return writer.text();
}

// ---------------------------------------------------------------------------
// Locking
// ---------------------------------------------------------------------------
Result<void> LedgerStore::acquire_lock() {
  if (!config_.take_exclusive_lock) {
    read_only_ = true;
    return ok();
  }
  const std::string lock_path = (std::filesystem::path(config_.path) / kLockName).string();
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  const std::uint32_t deadline_ms = config_.lock_timeout_ms;
  std::uint32_t waited_ms = 0;
  while (true) {
    handle = CreateFileA(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return make_error(ErrorCode::IoFailure, "cannot open the store lock file", lock_path);
    }
    OVERLAPPED overlapped{};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &overlapped) != 0) {
      lock_handle_ = handle;
      locked_ = true;
      return ok();
    }
    CloseHandle(handle);
    if (waited_ms >= deadline_ms) {
      return make_error(ErrorCode::StoreLocked,
                        "another process holds the exclusive store lock", lock_path);
    }
    Sleep(10);
    waited_ms += 10;
  }
#else
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return make_error(ErrorCode::IoFailure, "cannot open the store lock file", lock_path);
  }
  const std::uint32_t deadline_ms = config_.lock_timeout_ms;
  std::uint32_t waited_ms = 0;
  while (true) {
    struct flock lock {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    if (::fcntl(fd, F_SETLK, &lock) == 0) {
      lock_handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
      locked_ = true;
      return ok();
    }
    if (waited_ms >= deadline_ms) {
      ::close(fd);
      return make_error(ErrorCode::StoreLocked,
                        "another process holds the exclusive store lock", lock_path);
    }
    ::usleep(10000);
    waited_ms += 10;
  }
#endif
}

void LedgerStore::release_lock() {
  if (!locked_) {
    return;
  }
#ifdef _WIN32
  if (lock_handle_ != nullptr) {
    HANDLE handle = static_cast<HANDLE>(lock_handle_);
    OVERLAPPED overlapped{};
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
  }
#else
  if (lock_handle_ != nullptr) {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(lock_handle_));
    ::close(fd);
  }
#endif
  lock_handle_ = nullptr;
  locked_ = false;
}

// ---------------------------------------------------------------------------
// Segment IO
// ---------------------------------------------------------------------------
std::string LedgerStore::segment_path(std::uint64_t segment_id) const {
  char name[64];
  std::snprintf(name, sizeof(name), "%s%08llu%s", kSegmentPrefix,
                static_cast<unsigned long long>(segment_id), kSegmentSuffix);
  return (std::filesystem::path(config_.path) / name).string();
}

Result<void> LedgerStore::open_segment(std::uint64_t segment_id, TimePoint created_at) {
  const std::string path = segment_path(segment_id);
  std::FILE* file = std::fopen(path.c_str(), "wb+");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot create a store segment", path);
  }
  std::uint8_t header[kSegmentHeaderBytes] = {};
  std::memcpy(header, kSegmentMagic, sizeof(kSegmentMagic));
  put_u32(header + 8, kStoreFormatVersion);
  put_u64(header + 12, segment_id);
  put_u64(header + 20, static_cast<std::uint64_t>(created_at.nanos()));
  put_u32(header + 28, 0);
  if (!write_exact(file, header, sizeof(header))) {
    std::fclose(file);
    return make_error(ErrorCode::IoFailure, "cannot write the segment header", path);
  }
  segment_file_ = file;
  current_segment_id_ = segment_id;
  current_segment_bytes_ = kSegmentHeaderBytes;
  current_segment_records_ = 0;
  segment_hasher_.reset();
  SegmentInfo info;
  info.file = std::filesystem::path(path).filename().string();
  info.segment_id = segment_id;
  info.records = 0;
  info.bytes = kSegmentHeaderBytes;
  segments_.push_back(info);
  return ok();
}

Result<void> LedgerStore::write_frame(RecordKind kind, std::string_view payload,
                                      TimePoint written_at) {
  if (segment_file_ == nullptr) {
    FEL_TRY(open_segment(next_segment_id_, written_at));
    ++next_segment_id_;
  }
  if (payload.size() > limits::kMaxRecordBytes) {
    return make_error(ErrorCode::PayloadTooLarge, "record exceeds the payload budget");
  }
  std::string frame;
  frame.reserve(kRecordHeaderBytes + payload.size());
  std::uint8_t meta[kRecordHeaderBytes];
  meta[0] = static_cast<std::uint8_t>(kind);
  put_u64(meta + 1, current_segment_records_);
  put_u64(meta + 9, static_cast<std::uint64_t>(written_at.nanos()));
  frame.append(reinterpret_cast<const char*>(meta), sizeof(meta));
  frame.append(payload.data(), payload.size());

  std::uint8_t frame_header[kFrameHeaderBytes];
  put_u32(frame_header, static_cast<std::uint32_t>(frame.size()));
  put_u32(frame_header + 4, crc32c(frame.data(), frame.size()));

  if (!write_exact(segment_file_, frame_header, sizeof(frame_header)) ||
      !write_exact(segment_file_, frame.data(), frame.size())) {
    return make_error(ErrorCode::IoFailure, "cannot append a record to the store segment");
  }
  segment_hasher_.update(frame_header, sizeof(frame_header));
  segment_hasher_.update(frame.data(), frame.size());

  current_segment_bytes_ += kFrameHeaderBytes + frame.size();
  ++current_segment_records_;
  segments_.back().records = current_segment_records_;
  segments_.back().bytes = current_segment_bytes_;
  return ok();
}

Result<void> LedgerStore::seal_segment() {
  if (segment_file_ == nullptr) {
    return ok();
  }
  std::uint8_t footer[kFooterBytes] = {};
  std::memcpy(footer, kFooterMagic, sizeof(kFooterMagic));
  put_u64(footer + 8, current_segment_records_);
  const Digest256 digest = segment_hasher_.finish();
  std::memcpy(footer + 16, digest.bytes().data(), digest.bytes().size());
  put_u32(footer + 48, crc32c(footer, 48));
  if (!write_exact(segment_file_, footer, sizeof(footer))) {
    std::fclose(segment_file_);
    segment_file_ = nullptr;
    return make_error(ErrorCode::IoFailure, "cannot seal the store segment");
  }
  current_segment_bytes_ += kFooterBytes;
  segments_.back().bytes = current_segment_bytes_;
  segments_.back().digest = digest;
  segments_.back().damaged = false;
  sync_file(segment_file_);
  std::fclose(segment_file_);
  segment_file_ = nullptr;
  current_segment_records_ = 0;
  current_segment_bytes_ = 0;
  segment_hasher_.reset();
  return ok();
}

Result<void> LedgerStore::rotate_if_needed(std::uint64_t incoming_bytes) {
  if (segment_file_ == nullptr) {
    return ok();
  }
  const auto projected = add_checked(current_segment_bytes_, incoming_bytes);
  if (!projected.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "segment size arithmetic overflowed");
  }
  if (*projected <= config_.max_segment_bytes) {
    return ok();
  }
  FEL_TRY(seal_segment());
  if (segments_.size() >= config_.max_segments) {
    return make_error(ErrorCode::CapacityExceeded,
                      "segment budget exhausted; compact the store before appending");
  }
  FEL_TRY(open_segment(next_segment_id_, watermark_));
  ++next_segment_id_;
  return ok();
}

Result<void> LedgerStore::sweep_orphans() {
  std::error_code ec;
  const std::filesystem::path root(config_.path);
  if (!std::filesystem::exists(root, ec)) {
    return ok();
  }
  std::vector<std::string> known;
  known.reserve(segments_.size());
  for (const auto& info : segments_) {
    known.push_back(info.file);
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(kSegmentPrefix, 0) != 0) {
      continue;
    }
    if (name.size() < std::strlen(kSegmentSuffix) ||
        name.compare(name.size() - std::strlen(kSegmentSuffix), std::strlen(kSegmentSuffix),
                     kSegmentSuffix) != 0) {
      continue;
    }
    if (std::find(known.begin(), known.end(), name) != known.end()) {
      continue;
    }
    std::error_code remove_ec;
    std::filesystem::remove(entry.path(), remove_ec);
    if (!remove_ec) {
      ++recovery_.segments_orphaned;
      recovery_.diagnostics.push_back("removed orphaned segment " + name);
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Segment scanning
// ---------------------------------------------------------------------------
Result<void> LedgerStore::scan_segment(const std::string& file, SegmentInfo* info_ptr,
                                       const RecordVisitor* visitor) const {
  const std::string path = (std::filesystem::path(config_.path) / file).string();
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) {
    if (info_ptr != nullptr) {
      info_ptr->damaged = true;
    }
    return make_error(ErrorCode::SegmentMissing, "store segment is missing", path);
  }

  SegmentInfo local;
  local.file = file;
  std::uint8_t header[kSegmentHeaderBytes];
  if (!read_exact(handle, header, sizeof(header)) ||
      std::memcmp(header, kSegmentMagic, sizeof(kSegmentMagic)) != 0) {
    std::fclose(handle);
    if (info_ptr != nullptr) {
      info_ptr->damaged = true;
    }
    return make_error(ErrorCode::SegmentCorrupt, "store segment header is invalid", path);
  }
  const std::uint32_t version = get_u32(header + 8);
  if (version != kStoreFormatVersion) {
    std::fclose(handle);
    if (info_ptr != nullptr) {
      info_ptr->damaged = true;
    }
    return make_error(ErrorCode::StoreFormatMismatch, "store segment format version differs",
                      std::to_string(version));
  }
  local.segment_id = get_u64(header + 12);

  Sha256 hasher;
  std::uint64_t records = 0;
  std::uint64_t bytes = kSegmentHeaderBytes;
  bool truncated = false;
  bool sealed = false;
  std::string problem;

  while (true) {
    std::uint8_t frame_header[kFrameHeaderBytes];
    const std::size_t got = std::fread(frame_header, 1, sizeof(frame_header), handle);
    if (got == 0) {
      problem = "segment ends without a footer";
      truncated = true;
      break;
    }
    if (got != sizeof(frame_header)) {
      problem = "trailing partial frame header";
      truncated = true;
      break;
    }
    if (std::memcmp(frame_header, kFooterMagic, sizeof(kFooterMagic)) == 0) {
      std::uint8_t rest[44];
      if (!read_exact(handle, rest, sizeof(rest))) {
        problem = "truncated segment footer";
        truncated = true;
        break;
      }
      std::uint8_t footer[kFooterBytes];
      std::memcpy(footer, frame_header, sizeof(frame_header));
      std::memcpy(footer + 8, rest, sizeof(rest));
      if (crc32c(footer, 48) != get_u32(footer + 48)) {
        problem = "segment footer checksum mismatch";
        truncated = true;
        break;
      }
      const Digest256 actual = hasher.finish();
      if (actual != Digest256::from_hex(to_hex(footer + 16, 32))) {
        problem = "segment content digest does not match the footer";
        truncated = true;
        break;
      }
      if (get_u64(footer + 8) != records) {
        problem = "segment footer record count does not match the frames";
        truncated = true;
        break;
      }
      bytes += kFooterBytes;
      local.digest = actual;
      sealed = true;
      break;
    }

    const std::uint32_t length = get_u32(frame_header);
    const std::uint32_t crc = get_u32(frame_header + 4);
    if (length > limits::kMaxRecordBytes) {
      problem = "frame length exceeds the payload budget";
      truncated = true;
      break;
    }
    std::string frame(length, '\0');
    if (length > 0 && !read_exact(handle, frame.data(), length)) {
      problem = "truncated record payload";
      truncated = true;
      break;
    }
    if (crc32c(frame.data(), frame.size()) != crc) {
      problem = "record checksum mismatch";
      truncated = true;
      break;
    }
    if (length < kRecordHeaderBytes) {
      problem = "record is shorter than its mandatory header";
      truncated = true;
      break;
    }
    const auto kind_value = static_cast<std::uint8_t>(frame[0]);
    if (kind_value == 0 || kind_value > 8) {
      problem = "record kind is not recognised";
      truncated = true;
      break;
    }
    hasher.update(frame_header, sizeof(frame_header));
    hasher.update(frame.data(), frame.size());
    bytes += kFrameHeaderBytes + length;

    if (visitor != nullptr) {
      StoreRecord record;
      record.kind = static_cast<RecordKind>(kind_value);
      record.ordinal = get_u64(reinterpret_cast<const std::uint8_t*>(frame.data()) + 1);
      record.written_at = TimePoint::from_nanos(static_cast<std::int64_t>(
          get_u64(reinterpret_cast<const std::uint8_t*>(frame.data()) + 9)));
      record.payload = frame.substr(kRecordHeaderBytes);
      (*visitor)(record);
    }
    ++records;
  }

  const std::uint64_t total = file_size_of(handle);
  local.records = records;
  local.bytes = bytes;
  local.digest = sealed ? local.digest : Digest256{};
  local.damaged = truncated;
  std::fclose(handle);

  if (truncated) {
    const std::uint64_t tail = total > bytes ? total - bytes : 0;
    recovery_.truncated_at_damage = true;
    recovery_.tail_bytes_discarded += tail;
    recovery_.records_discarded += 0;
    if (problem.find("checksum") != std::string::npos) {
      ++recovery_.checksum_failures;
    }
    recovery_.diagnostics.push_back(path + ": " + problem);
  }

  if (info_ptr != nullptr) {
    const bool was_damaged = info_ptr->damaged;
    *info_ptr = local;
    info_ptr->damaged = was_damaged || local.damaged;
  }
  return ok();
}

void LedgerStore::recompute_totals() noexcept {
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  for (const auto& info : segments_) {
    records += info.records;
    bytes += info.bytes;
  }
  total_records_ = records;
  total_bytes_ = bytes;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
Result<void> LedgerStore::load_manifest() {
  const std::string path = (std::filesystem::path(config_.path) / kManifestName).string();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    recovery_.manifest_present = false;
    return make_error(ErrorCode::ManifestMissing, "store manifest is missing", path);
  }
  recovery_.manifest_present = true;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot open the store manifest", path);
  }
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
    if (text.size() > limits::kMaxRecordBytes) {
      std::fclose(file);
      return make_error(ErrorCode::PayloadTooLarge, "store manifest exceeds the payload budget");
    }
  }
  std::fclose(file);

  auto parsed = Json::parse(text);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::ManifestCorrupt, "store manifest is not valid JSON",
                      parsed.error().message);
  }
  Json document = parsed.value();
  if (!document.is_object()) {
    return make_error(ErrorCode::ManifestCorrupt, "store manifest is not an object");
  }
  const Json* integrity = document.find("integrity");
  if (integrity == nullptr || !integrity->is_string()) {
    return make_error(ErrorCode::ManifestCorrupt, "store manifest has no integrity field");
  }
  const std::string expected = integrity->as_string();
  document.as_object_mut().erase("integrity");
  const Digest256 actual = Sha256::hash(document.dump(0));
  if (actual.to_hex() != expected) {
    return make_error(ErrorCode::ManifestCorrupt,
                      "store manifest integrity check failed; the file was modified or damaged");
  }

  std::uint64_t version = 0;
  FEL_TRY_ASSIGN(version, document.require_u64("format_version"));
  if (version != kStoreFormatVersion) {
    return make_error(ErrorCode::StoreFormatMismatch, "store format version differs",
                      std::to_string(version));
  }
  std::string store_hex;
  FEL_TRY_ASSIGN(store_hex, document.require_string("store"));
  FEL_TRY_ASSIGN(store_id_, StoreId::parse(store_hex));
  FEL_TRY_ASSIGN(revision_, document.require_u64("revision"));
  FEL_TRY_ASSIGN(total_records_, document.require_u64("records"));
  FEL_TRY_ASSIGN(total_bytes_, document.require_u64("bytes"));
  if (const Json* value = document.find("watermark");
      value != nullptr && value->is_string() && value->as_string() != "-") {
    auto parsed_time = parse_rfc3339(value->as_string());
    if (parsed_time.has_value()) {
      watermark_ = parsed_time.value();
    }
  }
  if (const Json* value = document.find("first_write");
      value != nullptr && value->is_string() && value->as_string() != "-") {
    auto parsed_time = parse_rfc3339(value->as_string());
    if (parsed_time.has_value()) {
      first_write_ = parsed_time.value();
    }
  }
  const Json* segment_list = document.find("segments");
  if (segment_list == nullptr || !segment_list->is_array()) {
    return make_error(ErrorCode::ManifestCorrupt, "store manifest has no segment list");
  }
  segments_.clear();
  next_segment_id_ = 1;
  for (const auto& entry : segment_list->as_array()) {
    if (!entry.is_object()) {
      return make_error(ErrorCode::ManifestCorrupt, "segment entry is not an object");
    }
    SegmentInfo info;
    FEL_TRY_ASSIGN(info.file, entry.require_string("file"));
    FEL_TRY_ASSIGN(info.segment_id, entry.require_u64("id"));
    FEL_TRY_ASSIGN(info.records, entry.require_u64("records"));
    FEL_TRY_ASSIGN(info.bytes, entry.require_u64("bytes"));
    std::string digest;
    FEL_TRY_ASSIGN(digest, entry.require_string("digest"));
    info.digest = Digest256::from_hex(digest);
    segments_.push_back(info);
    if (info.segment_id >= next_segment_id_) {
      next_segment_id_ = info.segment_id + 1;
    }
  }
  recovery_.manifest_valid = true;
  return ok();
}

Result<void> LedgerStore::rebuild_manifest_from_segments() {
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(config_.path, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const std::size_t suffix_len = std::strlen(kSegmentSuffix);
    if (name.rfind(kSegmentPrefix, 0) == 0 && name.size() > suffix_len &&
        name.compare(name.size() - suffix_len, suffix_len, kSegmentSuffix) == 0) {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  segments_.clear();
  next_segment_id_ = 1;
  for (const auto& name : names) {
    SegmentInfo info;
    info.file = name;
    const auto outcome = scan_segment(name, &info, nullptr);
    if (!outcome.has_value()) {
      ++recovery_.segments_damaged;
      recovery_.diagnostics.push_back("segment " + name +
                                      " could not be scanned: " + outcome.error().message);
      continue;
    }
    if (info.damaged) {
      ++recovery_.segments_damaged;
    }
    if (info.segment_id >= next_segment_id_) {
      next_segment_id_ = info.segment_id + 1;
    }
    segments_.push_back(info);
  }
  std::sort(segments_.begin(), segments_.end(),
            [](const SegmentInfo& a, const SegmentInfo& b) { return a.segment_id < b.segment_id; });
  recovery_.manifest_rebuilt = true;
  manifest_dirty_ = true;
  return ok();
}

Result<void> LedgerStore::write_manifest() {
  Json root = Json::object();
  root.set("format_version", Json::number(static_cast<std::uint64_t>(kStoreFormatVersion)));
  root.set("store", Json::string(store_id_.to_string()));
  root.set("revision", Json::number(revision_));
  root.set("records", Json::number(total_records_));
  root.set("bytes", Json::number(total_bytes_));
  root.set("watermark",
           Json::string(watermark_.is_zero() ? std::string("-") : watermark_.to_rfc3339_millis()));
  root.set("first_write",
           Json::string(first_write_.is_zero() ? std::string("-")
                                               : first_write_.to_rfc3339_millis()));
  Json list = Json::array();
  for (const auto& info : segments_) {
    Json entry = Json::object();
    entry.set("file", Json::string(info.file));
    entry.set("id", Json::number(info.segment_id));
    entry.set("records", Json::number(info.records));
    entry.set("bytes", Json::number(info.bytes));
    entry.set("digest", Json::string(info.digest.to_hex()));
    list.push(std::move(entry));
  }
  root.set("segments", std::move(list));
  const Digest256 integrity = Sha256::hash(root.dump(0));
  root.set("integrity", Json::string(integrity.to_hex()));

  const std::string path = (std::filesystem::path(config_.path) / kManifestName).string();
  const std::string temp = path + ".tmp";
  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot create the store manifest", temp);
  }
  const std::string text = root.dump(0);
  if (!write_exact(file, text.data(), text.size())) {
    std::fclose(file);
    return make_error(ErrorCode::IoFailure, "cannot write the store manifest", temp);
  }
  sync_file(file);
  std::fclose(file);

  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(path, remove_ec);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
      return make_error(ErrorCode::IoFailure, "cannot publish the store manifest", ec.message());
    }
  }
  manifest_dirty_ = false;
  return ok();
}

// ---------------------------------------------------------------------------
// Open / replay / append / verify
// ---------------------------------------------------------------------------
Result<std::unique_ptr<LedgerStore>> LedgerStore::open(const StoreConfig& config) {
  if (config.path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "store path must not be empty");
  }
  std::unique_ptr<LedgerStore> store(new LedgerStore(config));
  std::error_code ec;
  const std::filesystem::path root(config.path);
  const bool existed = std::filesystem::exists(root, ec);
  if (!existed) {
    if (config.mode == StoreOpenMode::OpenExisting) {
      return make_error(ErrorCode::StoreNotFound, "store directory does not exist", config.path);
    }
    std::filesystem::create_directories(root, ec);
    if (ec) {
      return make_error(ErrorCode::IoFailure, "cannot create the store directory", config.path);
    }
  } else if (config.mode == StoreOpenMode::CreateNew) {
    return make_error(ErrorCode::InvalidArgument,
                      "store already exists and create-new was requested", config.path);
  }

  FEL_TRY(store->acquire_lock());

  auto manifest_status = store->load_manifest();
  if (!manifest_status.has_value()) {
    if (config.recovery == RecoveryPolicy::Strict &&
        manifest_status.error().code != ErrorCode::ManifestMissing) {
      return manifest_status.error();
    }
    store->recovery_.diagnostics.push_back("manifest not usable: " +
                                           manifest_status.error().message);
    FEL_TRY(store->rebuild_manifest_from_segments());
  } else {
    std::vector<SegmentInfo> verified;
    verified.reserve(store->segments_.size());
    for (const auto& info : store->segments_) {
      SegmentInfo copy;
      copy.file = info.file;
      copy.segment_id = info.segment_id;
      copy.digest = info.digest;
      const auto outcome = store->scan_segment(info.file, &copy, nullptr);
      if (!outcome.has_value() || copy.damaged || copy.records != info.records ||
          copy.digest != info.digest) {
        ++store->recovery_.segments_damaged;
        store->recovery_.diagnostics.push_back("segment " + info.file +
                                               " disagrees with the manifest");
        if (config.recovery == RecoveryPolicy::Strict) {
          return make_error(ErrorCode::SegmentCorrupt,
                            "segment disagrees with the manifest under strict recovery",
                            info.file);
        }
      }
      verified.push_back(copy);
    }
    store->segments_ = std::move(verified);
  }

  store->recovery_.segments_present = store->segments_.size();
  store->recovery_.segments_scanned = store->segments_.size();
  store->recovery_.watermark = store->watermark_;
  store->recovery_.conservative = config.recovery == RecoveryPolicy::Conservative;
  store->recompute_totals();
  store->recovery_.records_recovered = store->total_records_;
  store->recovery_.bytes_recovered = store->total_bytes_;
  for (const auto& info : store->segments_) {
    if (info.damaged) {
      ++store->recovery_.segments_damaged;
    }
  }
  FEL_TRY(store->sweep_orphans());
  if (store->manifest_dirty_) {
    FEL_TRY(store->write_manifest());
  }
  return std::unique_ptr<LedgerStore>(std::move(store));
}

LedgerStore::~LedgerStore() {
  if (segment_file_ != nullptr) {
    static_cast<void>(seal_segment());
    recompute_totals();
    manifest_dirty_ = true;
    static_cast<void>(write_manifest());
  }
  release_lock();
}

Result<void> LedgerStore::append(RecordKind kind, std::string payload, TimePoint written_at) {
  return append_many({{kind, std::move(payload)}}, written_at);
}

Result<void> LedgerStore::append_many(
    const std::vector<std::pair<RecordKind, std::string>>& records, TimePoint written_at) {
  if (records.empty()) {
    return ok();
  }
  if (read_only_) {
    return make_error(ErrorCode::StoreReadOnly, "the store was opened read only");
  }
  std::uint64_t incoming = 0;
  for (const auto& entry : records) {
    if (entry.second.size() > limits::kMaxRecordBytes) {
      return make_error(ErrorCode::PayloadTooLarge, "record exceeds the payload budget");
    }
    const auto with_frame = add_checked(entry.second.size(),
                                        static_cast<std::uint64_t>(kRecordHeaderBytes) +
                                            kFrameHeaderBytes);
    if (!with_frame.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow, "record size arithmetic overflowed");
    }
    const auto total = add_checked(incoming, *with_frame);
    if (!total.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow, "batch size arithmetic overflowed");
    }
    incoming = *total;
  }
  const auto projected = add_checked(total_bytes_, incoming);
  if (!projected.has_value()) {
    return make_error(ErrorCode::ArithmeticOverflow, "store size arithmetic overflowed");
  }
  if (*projected > config_.max_bytes) {
    return make_error(ErrorCode::CapacityExceeded, "store size budget exhausted",
                      std::to_string(*projected) + " > " + std::to_string(config_.max_bytes));
  }

  if (segment_file_ == nullptr) {
    if (segments_.size() >= config_.max_segments) {
      return make_error(ErrorCode::CapacityExceeded,
                        "segment budget exhausted; compact the store before appending");
    }
    FEL_TRY(open_segment(next_segment_id_, written_at));
    ++next_segment_id_;
  }
  for (const auto& entry : records) {
    FEL_TRY(rotate_if_needed(static_cast<std::uint64_t>(entry.second.size()) +
                             kRecordHeaderBytes + kFrameHeaderBytes));
    FEL_TRY(write_frame(entry.first, entry.second, written_at));
  }
  if (watermark_.is_zero() || written_at > watermark_) {
    watermark_ = written_at;
  }
  if (first_write_.is_zero()) {
    first_write_ = written_at;
  }
  FEL_TRY(seal_segment());
  recompute_totals();
  ++revision_;
  manifest_dirty_ = true;
  return flush();
}

Result<void> LedgerStore::flush() {
  if (manifest_dirty_) {
    FEL_TRY(write_manifest());
  }
  return ok();
}

Result<std::uint64_t> LedgerStore::replay(const RecordVisitor& visitor) const {
  std::uint64_t visited = 0;
  for (const auto& info : segments_) {
    const RecordVisitor counting = [&](const StoreRecord& record) {
      ++visited;
      visitor(record);
    };
    const auto outcome = scan_segment(info.file, nullptr, &counting);
    if (!outcome.has_value() && config_.recovery == RecoveryPolicy::Strict) {
      return outcome.error();
    }
  }
  return visited;
}

Result<RecoveryReport> LedgerStore::verify() const {
  RecoveryReport report;
  report.manifest_present = recovery_.manifest_present;
  report.manifest_valid = recovery_.manifest_valid;
  report.manifest_rebuilt = recovery_.manifest_rebuilt;
  report.conservative = recovery_.conservative;
  for (const auto& info : segments_) {
    SegmentInfo observed;
    observed.file = info.file;
    observed.segment_id = info.segment_id;
    const auto outcome = scan_segment(info.file, &observed, nullptr);
    ++report.segments_scanned;
    if (!outcome.has_value()) {
      ++report.segments_missing;
      report.diagnostics.push_back(info.file + ": " + outcome.error().message);
      continue;
    }
    if (observed.damaged || observed.digest != info.digest || observed.records != info.records ||
        observed.bytes != info.bytes) {
      ++report.segments_damaged;
      report.diagnostics.push_back(info.file + ": segment content differs from the manifest");
      continue;
    }
    report.records_recovered += observed.records;
    report.bytes_recovered += observed.bytes;
  }
  report.segments_present = segments_.size();
  report.watermark = watermark_;
  return report;
}

Result<CompactionReport> LedgerStore::compact(
    const std::function<bool(const StoreRecord&)>& keep, TimePoint now) {
  CompactionReport report;
  if (read_only_) {
    return make_error(ErrorCode::StoreReadOnly, "the store was opened read only");
  }
  if (keep == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "compaction requires a retention predicate");
  }
  report.segments_before = segments_.size();
  report.bytes_before = total_bytes_;

  std::vector<std::pair<RecordKind, std::string>> survivors;
  std::uint64_t dropped = 0;
  const RecordVisitor collector = [&](const StoreRecord& record) {
    if (keep(record)) {
      survivors.emplace_back(record.kind, record.payload);
    } else {
      ++dropped;
    }
  };
  for (const auto& info : segments_) {
    FEL_TRY(scan_segment(info.file, nullptr, &collector));
  }

  std::vector<std::string> old_files;
  old_files.reserve(segments_.size());
  for (const auto& info : segments_) {
    old_files.push_back(info.file);
  }
  segments_.clear();
  total_records_ = 0;
  total_bytes_ = 0;

  if (!survivors.empty()) {
    FEL_TRY(open_segment(next_segment_id_, now));
    ++next_segment_id_;
    for (const auto& entry : survivors) {
      FEL_TRY(rotate_if_needed(static_cast<std::uint64_t>(entry.second.size()) +
                               kRecordHeaderBytes + kFrameHeaderBytes));
      FEL_TRY(write_frame(entry.first, entry.second, now));
    }
    FEL_TRY(seal_segment());
  }
  recompute_totals();
  ++revision_;
  manifest_dirty_ = true;
  FEL_TRY(write_manifest());

  for (const auto& name : old_files) {
    std::error_code remove_ec;
    std::filesystem::remove(std::filesystem::path(config_.path) / name, remove_ec);
    if (remove_ec) {
      report.diagnostics.push_back("could not remove " + name + ": " + remove_ec.message());
    }
  }
  report.performed = true;
  report.segments_after = segments_.size();
  report.bytes_after = total_bytes_;
  report.records_dropped = dropped;
  return report;
}

StoreStats LedgerStore::stats() const {
  StoreStats out;
  out.records = total_records_;
  out.bytes = total_bytes_;
  out.segments = segments_.size();
  out.revision = revision_;
  out.watermark = watermark_;
  out.first_write = first_write_;
  out.read_only = read_only_;
  out.locked = locked_;
  return out;
}

}  // namespace fel
