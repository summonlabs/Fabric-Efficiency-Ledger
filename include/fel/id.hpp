// Fabric Efficiency Ledger - strongly typed domain identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

#include "fel/crypto.hpp"
#include "fel/error.hpp"

namespace fel {

// A 128 bit raw identity. Identities are content derived (SHA-256 truncated to
// 128 bits) so that identical definitions collapse to the same identity no
// matter where or when they were declared, and different definitions cannot
// silently alias.
struct RawId {
  std::array<std::uint8_t, 16> bytes{};

  friend bool operator==(const RawId& a, const RawId& b) noexcept { return a.bytes == b.bytes; }
  friend bool operator!=(const RawId& a, const RawId& b) noexcept { return !(a == b); }
  friend bool operator<(const RawId& a, const RawId& b) noexcept { return a.bytes < b.bytes; }
};

// Tag types. Each one is a distinct C++ type, so a ResourceId can never be
// passed where a ScopeId is expected.
struct ResourceTag {
  static constexpr std::string_view kind_name = "resource";
};
struct FlowTag {
  static constexpr std::string_view kind_name = "flow";
};
struct PathTag {
  static constexpr std::string_view kind_name = "path";
};
struct ReservationTag {
  static constexpr std::string_view kind_name = "reservation";
};
struct SourceTag {
  static constexpr std::string_view kind_name = "source";
};
struct GenerationTag {
  static constexpr std::string_view kind_name = "generation";
};
struct IncarnationTag {
  static constexpr std::string_view kind_name = "incarnation";
};
struct PeriodTag {
  static constexpr std::string_view kind_name = "period";
};
struct EvidenceTag {
  static constexpr std::string_view kind_name = "evidence";
};
struct OriginTag {
  static constexpr std::string_view kind_name = "origin";
};
struct ScopeTag {
  static constexpr std::string_view kind_name = "scope";
};
struct CorrectionTag {
  static constexpr std::string_view kind_name = "correction";
};
struct PolicyTag {
  static constexpr std::string_view kind_name = "policy-revision";
};
struct TopologyTag {
  static constexpr std::string_view kind_name = "topology-revision";
};
struct StoreTag {
  static constexpr std::string_view kind_name = "store";
};
struct ClaimTag {
  static constexpr std::string_view kind_name = "claim";
};
struct BindingTag {
  static constexpr std::string_view kind_name = "binding";
};

template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() = default;
  explicit StrongId(RawId raw) noexcept : raw_(raw) {}

  [[nodiscard]] static StrongId from_raw(RawId raw) noexcept { return StrongId{raw}; }
  [[nodiscard]] static StrongId nil() noexcept { return StrongId{}; }

  // Deterministic derivation. The tag name is absorbed first, then every part
  // length-prefixed, so the mapping (parts -> id) is injective.
  [[nodiscard]] static StrongId derive(std::initializer_list<std::string_view> parts) {
    Sha256 hasher;
    hasher.update_field(Tag::kind_name);
    for (const auto& part : parts) {
      hasher.update_field(part);
    }
    const Digest256 digest = hasher.finish();
    RawId raw;
    for (std::size_t i = 0; i < raw.bytes.size(); ++i) {
      raw.bytes[i] = digest.bytes()[i];
    }
    return StrongId{raw};
  }

  [[nodiscard]] static StrongId derive(std::string_view a, std::string_view b) {
    return derive({a, b});
  }
  [[nodiscard]] static StrongId derive(std::string_view a) { return derive({a}); }

  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    if (!is_hex(text, 32)) {
      return make_error(ErrorCode::MalformedIdentity,
                        std::string("identity must be 32 lowercase hex characters: ") +
                            std::string(Tag::kind_name),
                        std::string(text));
    }
    RawId raw;
    if (!from_hex(text, raw.bytes.data(), raw.bytes.size())) {
      return make_error(ErrorCode::MalformedIdentity, "identity hex decode failed",
                        std::string(text));
    }
    return StrongId{raw};
  }

  [[nodiscard]] const RawId& raw() const noexcept { return raw_; }
  [[nodiscard]] bool is_nil() const noexcept {
    for (const auto byte : raw_.bytes) {
      if (byte != 0) return false;
    }
    return true;
  }

  [[nodiscard]] std::string to_string() const {
    return to_hex(raw_.bytes.data(), raw_.bytes.size());
  }
  [[nodiscard]] std::string_view kind_name() const noexcept { return Tag::kind_name; }

  friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.raw_ == b.raw_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return !(a == b); }
  friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.raw_ < b.raw_; }
  friend bool operator>(const StrongId& a, const StrongId& b) noexcept { return b < a; }
  friend bool operator<=(const StrongId& a, const StrongId& b) noexcept { return !(b < a); }
  friend bool operator>=(const StrongId& a, const StrongId& b) noexcept { return !(a < b); }

 private:
  RawId raw_{};
};

using ResourceId = StrongId<ResourceTag>;
using FlowId = StrongId<FlowTag>;
using PathId = StrongId<PathTag>;
using ReservationId = StrongId<ReservationTag>;
using SourceId = StrongId<SourceTag>;
using GenerationId = StrongId<GenerationTag>;
using IncarnationId = StrongId<IncarnationTag>;
using PeriodId = StrongId<PeriodTag>;
using EvidenceId = StrongId<EvidenceTag>;
using OriginId = StrongId<OriginTag>;
using ScopeId = StrongId<ScopeTag>;
using CorrectionId = StrongId<CorrectionTag>;
using PolicyRevisionId = StrongId<PolicyTag>;
using TopologyRevisionId = StrongId<TopologyTag>;
using StoreId = StrongId<StoreTag>;
using ClaimId = StrongId<ClaimTag>;
using BindingId = StrongId<BindingTag>;

// A content derived accounting identity. 256 bits because it names a cell of
// the accounting space and must not collide across large fabrics.
class AccountingIdentity {
 public:
  AccountingIdentity() = default;
  explicit AccountingIdentity(Digest256 digest) : digest_(digest) {}

  [[nodiscard]] static AccountingIdentity from_digest(Digest256 digest) {
    return AccountingIdentity{digest};
  }
  [[nodiscard]] static Result<AccountingIdentity> parse(std::string_view text) {
    if (!is_hex(text, 64)) {
      return make_error(ErrorCode::MalformedIdentity,
                        "accounting identity must be 64 lowercase hex characters",
                        std::string(text));
    }
    return AccountingIdentity{Digest256::from_hex(text)};
  }

  [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }
  [[nodiscard]] std::string to_string() const { return digest_.to_hex(); }

  friend bool operator==(const AccountingIdentity& a, const AccountingIdentity& b) noexcept {
    return a.digest_ == b.digest_;
  }
  friend bool operator!=(const AccountingIdentity& a, const AccountingIdentity& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const AccountingIdentity& a, const AccountingIdentity& b) noexcept {
    return a.digest_ < b.digest_;
  }

 private:
  Digest256 digest_{};
};

// Strongly typed 64 bit counters. Epoch, sequence, revision and ordinal
// confusion is a classic source of silent accounting error, so they are all
// distinct types.
struct EpochTag {
  static constexpr std::string_view kind_name = "epoch";
};
struct SequenceTag {
  static constexpr std::string_view kind_name = "sequence";
};
struct RevisionTag {
  static constexpr std::string_view kind_name = "revision";
};
struct OrdinalTag {
  static constexpr std::string_view kind_name = "ordinal";
};
struct AuthorityTag {
  static constexpr std::string_view kind_name = "authority";
};
struct BootTag {
  static constexpr std::string_view kind_name = "boot";
};

template <class Tag>
class StrongU64 {
 public:
  using tag_type = Tag;

  constexpr StrongU64() = default;
  explicit constexpr StrongU64(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  constexpr StrongU64& operator++() noexcept {
    ++value_;
    return *this;
  }
  constexpr StrongU64 operator++(int) noexcept {
    StrongU64 copy{value_};
    ++value_;
    return copy;
  }

  friend constexpr bool operator==(StrongU64 a, StrongU64 b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongU64 a, StrongU64 b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongU64 a, StrongU64 b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(StrongU64 a, StrongU64 b) noexcept { return b.value_ < a.value_; }
  friend constexpr bool operator<=(StrongU64 a, StrongU64 b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(StrongU64 a, StrongU64 b) noexcept { return !(a < b); }

 private:
  std::uint64_t value_ = 0;
};

using EpochId = StrongU64<EpochTag>;
using SourceSequence = StrongU64<SequenceTag>;
using RevisionOrdinal = StrongU64<RevisionTag>;
using Ordinal = StrongU64<OrdinalTag>;
using AuthorityRank = StrongU64<AuthorityTag>;
using BootId = StrongU64<BootTag>;

}  // namespace fel

namespace std {

template <class Tag>
struct hash<fel::StrongId<Tag>> {
  [[nodiscard]] size_t operator()(const fel::StrongId<Tag>& id) const noexcept {
    size_t acc = 1469598103934665603ULL;
    for (const auto byte : id.raw().bytes) {
      acc ^= static_cast<size_t>(byte);
      acc *= 1099511628211ULL;
    }
    return acc;
  }
};

template <>
struct hash<fel::AccountingIdentity> {
  [[nodiscard]] size_t operator()(const fel::AccountingIdentity& id) const noexcept {
    size_t acc = 1469598103934665603ULL;
    for (const auto byte : id.digest().bytes()) {
      acc ^= static_cast<size_t>(byte);
      acc *= 1099511628211ULL;
    }
    return acc;
  }
};

template <>
struct hash<fel::Digest256> {
  [[nodiscard]] size_t operator()(const fel::Digest256& id) const noexcept {
    size_t acc = 1469598103934665603ULL;
    for (const auto byte : id.bytes()) {
      acc ^= static_cast<size_t>(byte);
      acc *= 1099511628211ULL;
    }
    return acc;
  }
};

}  // namespace std
