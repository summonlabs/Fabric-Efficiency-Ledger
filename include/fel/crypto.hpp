// Fabric Efficiency Ledger - in-tree SHA-256 and CRC-32C (no external deps).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fel {

// A 256 bit digest. Used for accounting identities, content digests, manifest
// integrity, and explanation fingerprints. Comparable and hashable so it can be
// used as an ordered container key: determinism requires a total order.
class Digest256 {
 public:
  using Bytes = std::array<std::uint8_t, 32>;

  Digest256() = default;
  explicit Digest256(Bytes bytes) : bytes_(bytes) {}

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static Digest256 from_hex(std::string_view hex);

  friend bool operator==(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest256& a, const Digest256& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ < b.bytes_;
  }
  friend bool operator>(const Digest256& a, const Digest256& b) noexcept { return b < a; }
  friend bool operator<=(const Digest256& a, const Digest256& b) noexcept { return !(b < a); }
  friend bool operator>=(const Digest256& a, const Digest256& b) noexcept { return !(a < b); }

 private:
  Bytes bytes_{};
};

// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  // Length prefixed field absorption: the length is absorbed first so that
  // ("ab","c") and ("a","bc") cannot collide. Canonical hashing depends on it.
  void update_field(std::string_view text) noexcept;
  void update_u64(std::uint64_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  [[nodiscard]] Digest256 finish();

  [[nodiscard]] static Digest256 hash(std::string_view text);

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

// CRC-32C (Castagnoli), used for record framing integrity.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(text.data(), text.size());
}

[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size);
[[nodiscard]] inline std::string to_hex(std::string_view text) {
  return to_hex(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}
[[nodiscard]] bool from_hex(std::string_view hex, std::uint8_t* out, std::size_t out_size) noexcept;
[[nodiscard]] bool is_hex(std::string_view text, std::size_t exact_len) noexcept;

}  // namespace fel
