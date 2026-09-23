// Fabric Efficiency Ledger - explicit, integer, monotonic-safe time types.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fel/error.hpp"

namespace fel {

// Nanoseconds since the Unix epoch, UTC, as a signed 64 bit integer. All time in
// the ledger is exact integer arithmetic: no floating point, no implicit
// rounding, no locale dependent formatting.
class TimePoint {
 public:
  constexpr TimePoint() = default;
  explicit constexpr TimePoint(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr TimePoint from_nanos(std::int64_t nanos) noexcept {
    return TimePoint{nanos};
  }
  [[nodiscard]] static constexpr TimePoint from_micros(std::int64_t micros) noexcept {
    return TimePoint{micros * 1000};
  }
  [[nodiscard]] static constexpr TimePoint from_millis(std::int64_t millis) noexcept {
    return TimePoint{millis * 1000000};
  }
  [[nodiscard]] static constexpr TimePoint from_seconds(std::int64_t seconds) noexcept {
    return TimePoint{seconds * 1000000000};
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return nanos_ / 1000000; }

  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  friend constexpr bool operator==(TimePoint a, TimePoint b) noexcept { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(TimePoint a, TimePoint b) noexcept { return a.nanos_ != b.nanos_; }
  friend constexpr bool operator<(TimePoint a, TimePoint b) noexcept { return a.nanos_ < b.nanos_; }
  friend constexpr bool operator>(TimePoint a, TimePoint b) noexcept { return b < a; }
  friend constexpr bool operator<=(TimePoint a, TimePoint b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(TimePoint a, TimePoint b) noexcept { return !(a < b); }

  [[nodiscard]] std::string to_rfc3339() const;
  [[nodiscard]] std::string to_rfc3339_millis() const;

 private:
  std::int64_t nanos_ = 0;
};

// Signed duration in nanoseconds.
class Duration {
 public:
  constexpr Duration() = default;
  explicit constexpr Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) noexcept {
    return Duration{nanos};
  }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t micros) noexcept {
    return Duration{micros * 1000};
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) noexcept {
    return Duration{millis * 1000000};
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) noexcept {
    return Duration{seconds * 1000000000};
  }
  [[nodiscard]] static constexpr Duration from_minutes(std::int64_t minutes) noexcept {
    return Duration{minutes * 60 * 1000000000};
  }
  [[nodiscard]] static constexpr Duration from_hours(std::int64_t hours) noexcept {
    return Duration{hours * 3600 * 1000000000};
  }
  [[nodiscard]] static constexpr Duration from_days(std::int64_t days) noexcept {
    return Duration{days * 86400 * 1000000000};
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return nanos_ / 1000000; }
  [[nodiscard]] constexpr std::int64_t seconds() const noexcept { return nanos_ / 1000000000; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  [[nodiscard]] std::string to_iso8601() const;

  friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept { return a.nanos_ != b.nanos_; }
  friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.nanos_ < b.nanos_; }
  friend constexpr bool operator>(Duration a, Duration b) noexcept { return b < a; }
  friend constexpr bool operator<=(Duration a, Duration b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Duration a, Duration b) noexcept { return !(a < b); }

  friend constexpr Duration operator+(Duration a, Duration b) noexcept {
    return Duration{a.nanos_ + b.nanos_};
  }
  friend constexpr Duration operator-(Duration a, Duration b) noexcept {
    return Duration{a.nanos_ - b.nanos_};
  }

 private:
  std::int64_t nanos_ = 0;
};

[[nodiscard]] inline TimePoint operator+(TimePoint t, Duration d) noexcept {
  return TimePoint::from_nanos(t.nanos() + d.nanos());
}
[[nodiscard]] inline TimePoint operator-(TimePoint t, Duration d) noexcept {
  return TimePoint::from_nanos(t.nanos() - d.nanos());
}
[[nodiscard]] inline Duration operator-(TimePoint a, TimePoint b) noexcept {
  return Duration::from_nanos(a.nanos() - b.nanos());
}

// RFC 3339 / ISO 8601 with an explicit offset, or the literal "Z". Sub-second
// precision is accepted. Leap seconds are rejected (the ledger is not a clock).
[[nodiscard]] Result<TimePoint> parse_rfc3339(std::string_view text);

// Clock abstraction. Production uses the system clock; tests and deterministic
// replays inject a manual clock. The ledger never calls the wall clock on its
// own: an explicit observation time is always supplied by the caller.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual TimePoint now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimePoint now() const override;
};

class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimePoint start = TimePoint{}) : now_(start) {}
  [[nodiscard]] TimePoint now() const override { return now_; }
  void set(TimePoint value) noexcept { now_ = value; }
  void advance(Duration delta) noexcept { now_ = now_ + delta; }

 private:
  TimePoint now_;
};

}  // namespace fel
