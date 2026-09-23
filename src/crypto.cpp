// Fabric Efficiency Ledger - in-tree SHA-256 (FIPS 180-4) and CRC-32C (Castagnoli).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fel/crypto.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fel {
namespace {

// Lowercase alphabet used by every textual encoding in this file.
constexpr char kHexDigits[] = "0123456789abcdef";

// Numeric value of one hexadecimal digit, or -1 when the character is not one.
[[nodiscard]] constexpr int hex_digit_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kLengthFieldOffset = 56;

// FIPS 180-4 section 5.3.3: initial hash value H(0).
constexpr std::array<std::uint32_t, 8> kInitialState{
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

// FIPS 180-4 section 4.2.2: the sixty-four round constants.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

// CRC-32C (Castagnoli) reflected lookup table.
// The normal polynomial is 0x1EDC6F41 and its reflected form is 0x82F63B78.
// The table is produced during compilation, so there is no load time
// initialisation, no global mutable state, and no shared lazy construction.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = static_cast<std::uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      value = ((value & 1u) != 0u) ? static_cast<std::uint32_t>((value >> 1) ^ 0x82F63B78u)
                                   : static_cast<std::uint32_t>(value >> 1);
    }
    table[i] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace

// ---------------------------------------------------------------------------
// Sha256
// ---------------------------------------------------------------------------

void Sha256::reset() noexcept {
  state_ = kInitialState;
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  // Message schedule: the first sixteen words are the big endian block words,
  // the remaining forty-eight are derived by the FIPS 180-4 recurrence.
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    const std::size_t offset = i * 4;
    schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                  (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                  (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                  static_cast<std::uint32_t>(block[offset + 3]);
  }
  for (std::size_t i = 16; i < schedule.size(); ++i) {
    const std::uint32_t s0 = std::rotr(schedule[i - 15], 7) ^ std::rotr(schedule[i - 15], 18) ^
                             (schedule[i - 15] >> 3);
    const std::uint32_t s1 = std::rotr(schedule[i - 2], 17) ^ std::rotr(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < schedule.size(); ++i) {
    const std::uint32_t big_sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + big_sigma1 + choose + kRoundConstants[i] + schedule[i];
    const std::uint32_t big_sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (size == 0) {
    return;  // A zero length update is legal even with a null pointer.
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;

  // 1. Top up an already buffered partial block.
  if (buffered_ != 0) {
    const std::size_t needed = kBlockSize - buffered_;
    const std::size_t take = (size < needed) ? size : needed;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ == kBlockSize) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  // 2. Consume whole blocks straight out of the caller's buffer.
  while (size >= kBlockSize) {
    compress(bytes);
    bytes += kBlockSize;
    size -= kBlockSize;
  }

  // 3. Buffer the tail.
  if (size != 0) {
    std::memcpy(buffer_.data(), bytes, size);
    buffered_ = size;
  }
}

void Sha256::update_field(std::string_view text) noexcept {
  // Canonical field framing: the field length is absorbed as a fixed eight byte
  // BIG ENDIAN count immediately before the field bytes. Absorbing the length
  // first makes the mapping (field sequence -> digest input) injective, so
  // ("ab","c") and ("a","bc") can never produce the same hash input. Big endian
  // matches the bit length encoding SHA-256 itself appends to a message, and
  // update_u64/update_u32/update_byte deliberately use fixed width little
  // endian instead; the two framings are never mixed within one field.
  const std::uint64_t length = static_cast<std::uint64_t>(text.size());
  std::uint8_t prefix[8];
  for (std::size_t i = 0; i < sizeof(prefix); ++i) {
    const std::size_t shift = 8 * (sizeof(prefix) - 1 - i);
    prefix[i] = static_cast<std::uint8_t>((length >> shift) & 0xFFu);
  }
  update(prefix, sizeof(prefix));
  update(text.data(), text.size());
}

void Sha256::update_u64(std::uint64_t value) noexcept {
  std::uint8_t bytes[8];
  for (std::size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
  update(bytes, sizeof(bytes));
}

void Sha256::update_u32(std::uint32_t value) noexcept {
  std::uint8_t bytes[4];
  for (std::size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
  update(bytes, sizeof(bytes));
}

void Sha256::update_byte(std::uint8_t value) noexcept { update(&value, 1); }

Digest256 Sha256::finish() {
  // Padding: a single 0x80 byte, zeros, then the message bit length as a big
  // endian 64 bit integer in the final eight bytes of the last block.
  const std::uint64_t bit_length = total_bytes_ * 8u;
  buffer_[buffered_] = 0x80u;
  ++buffered_;
  if (buffered_ > kLengthFieldOffset) {
    while (buffered_ < kBlockSize) {
      buffer_[buffered_] = 0u;
      ++buffered_;
    }
    compress(buffer_.data());
    buffered_ = 0;
  }
  while (buffered_ < kLengthFieldOffset) {
    buffer_[buffered_] = 0u;
    ++buffered_;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    const std::size_t shift = 8 * (7 - i);
    buffer_[kLengthFieldOffset + i] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu);
  }
  compress(buffer_.data());
  buffered_ = 0;

  Digest256::Bytes digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    const std::uint32_t word = state_[i];
    digest[i * 4] = static_cast<std::uint8_t>((word >> 24) & 0xFFu);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((word >> 16) & 0xFFu);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((word >> 8) & 0xFFu);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(word & 0xFFu);
  }
  return Digest256{digest};
}

Digest256 Sha256::hash(std::string_view text) {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    const std::size_t index = static_cast<std::size_t>((crc ^ bytes[i]) & 0xFFu);
    crc = kCrc32cTable[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Hexadecimal text helpers
// ---------------------------------------------------------------------------

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = data[i];
    out[i * 2] = kHexDigits[byte >> 4];
    out[i * 2 + 1] = kHexDigits[byte & 0x0Fu];
  }
  return out;
}

bool from_hex(std::string_view hex, std::uint8_t* out, std::size_t out_size) noexcept {
  // Dividing instead of multiplying keeps this total for absurd out_size values.
  if ((hex.size() % 2) != 0 || (hex.size() / 2) != out_size) {
    return false;
  }
  if (out_size != 0 && out == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < out_size; ++i) {
    const int high = hex_digit_value(hex[i * 2]);
    const int low = hex_digit_value(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool is_hex(std::string_view text, std::size_t exact_len) noexcept {
  if (text.size() != exact_len) {
    return false;
  }
  for (const char c : text) {
    if (hex_digit_value(c) < 0) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Digest256
// ---------------------------------------------------------------------------

std::string Digest256::to_hex() const { return fel::to_hex(bytes_.data(), bytes_.size()); }

Digest256 Digest256::from_hex(std::string_view hex) {
  // Value returning API with no error channel: malformed input yields the zero
  // digest. Validation is still exact and allocation free.
  Bytes bytes{};
  // is_hex counts hex characters, so a 32 byte digest needs 64 of them.
  if (!is_hex(hex, bytes.size() * 2)) {
    return Digest256{};
  }
  if (!fel::from_hex(hex, bytes.data(), bytes.size())) {
    return Digest256{};
  }
  return Digest256{bytes};
}

}  // namespace fel
