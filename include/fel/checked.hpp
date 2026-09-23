// Fabric Efficiency Ledger - checked arithmetic and hard resource bounds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "fel/error.hpp"
#include "fel/limits.hpp"

namespace fel {

// ---------------------------------------------------------------------------
// Checked arithmetic
//
// Every size, count, or amount that can be influenced by external evidence or
// by operator supplied limits goes through these helpers. Overflow is a hard
// failure, never a wrap.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::optional<std::uint64_t> add_checked(std::uint64_t a,
                                                                std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return std::nullopt;
  }
  return a + b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> sub_checked(std::uint64_t a,
                                                                std::uint64_t b) noexcept {
  if (b > a) {
    return std::nullopt;
  }
  return a - b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> mul_checked(std::uint64_t a,
                                                                std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::uint64_t{0};
  }
  if (a > std::numeric_limits<std::uint64_t>::max() / b) {
    return std::nullopt;
  }
  return a * b;
}

[[nodiscard]] constexpr std::optional<std::int64_t> add_checked_i64(std::int64_t a,
                                                                   std::int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return std::nullopt;
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return std::nullopt;
  }
  return a + b;
}

[[nodiscard]] constexpr std::optional<std::int64_t> mul_checked_i64(std::int64_t a,
                                                                   std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::int64_t{0};
  }
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  if (a > 0) {
    if (b > 0) {
      if (a > max / b) return std::nullopt;
    } else {
      if (b < min / a) return std::nullopt;
    }
  } else {
    if (b > 0) {
      if (a < min / b) return std::nullopt;
    } else {
      if (a != 0 && b < max / a) return std::nullopt;
    }
  }
  return a * b;
}

// Accumulator that refuses to silently saturate. Used for every aggregate.
class CheckedSum {
 public:
  CheckedSum() = default;

  [[nodiscard]] Result<void> add(std::uint64_t value) {
    const auto next = add_checked(total_, value);
    if (!next.has_value()) {
      return make_error(ErrorCode::ArithmeticOverflow, "aggregate total overflowed 64 bits");
    }
    total_ = *next;
    ++additions_;
    return ok();
  }

  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t additions() const noexcept { return additions_; }
  [[nodiscard]] bool empty() const noexcept { return additions_ == 0; }

 private:
  std::uint64_t total_ = 0;
  std::uint64_t additions_ = 0;
};


// Result set envelope: every list shaped result reports whether it was clipped.
struct Truncation {
  bool truncated = false;
  std::uint64_t total_available = 0;
  std::uint64_t returned = 0;
  std::string reason;

  [[nodiscard]] bool empty() const noexcept { return returned == 0; }
};

}  // namespace fel
