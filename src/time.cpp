// Fabric Efficiency Ledger - RFC 3339 parsing/formatting and ISO 8601 durations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fel/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fel {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerDay = 86400;

// parse_rfc3339 refuses anything longer than this before touching the content.
constexpr std::size_t kMaxTimestampLength = 64;

// Sub-second precision beyond nanoseconds cannot be represented.
constexpr std::size_t kMaxFractionDigits = 9;

// Exact window that is representable as signed 64 bit nanoseconds. The first
// representable instant is 1677-09-21T00:12:43.145224192Z and the last is
// 2262-04-11T23:47:16.854775807Z; the two boundary seconds therefore accept
// only a partial nanosecond fraction. Anything outside the window is rejected
// with MalformedTimestamp instead of overflowing.
constexpr std::int64_t kMaxRepresentableSeconds = 9223372036;
constexpr std::int64_t kMinRepresentableSeconds = -9223372036;
constexpr std::int64_t kMaxFractionAtUpperBound = 854775807;
constexpr std::int64_t kMinFractionAtLowerBound = 145224192;

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool is_leap_year(std::int64_t year) noexcept {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

[[nodiscard]] constexpr int days_in_month(std::int64_t year, int month) noexcept {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return is_leap_year(year) ? 29 : 28;
    default:
      return 0;
  }
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Exact for the whole std::int64_t year range, no branches on
// the sign of the result and no undefined arithmetic.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, int month, int day) noexcept {
  year -= (month <= 2) ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const std::int64_t year_of_era = year - era * 400;  // [0, 399]
  const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const std::int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

// Inverse of days_from_civil (Howard Hinnant's civil_from_days).
constexpr void civil_from_days(std::int64_t days, std::int64_t& year, int& month, int& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t day_of_era = days - era * 146097;  // [0, 146096]
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;  // [0, 399]
  const std::int64_t full_year = year_of_era + era * 400;
  const std::int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);  // [0, 365]
  const std::int64_t march_month = (5 * day_of_year + 2) / 153;               // [0, 11]
  const std::int64_t day_of_month = day_of_year - (153 * march_month + 2) / 5 + 1;  // [1, 31]
  const std::int64_t month_of_year = march_month + (march_month < 10 ? 3 : -9);     // [1, 12]
  year = full_year + (month_of_year <= 2 ? 1 : 0);
  month = static_cast<int>(month_of_year);
  day = static_cast<int>(day_of_month);
}

// Reads exactly `count` decimal digits starting at `offset`.
[[nodiscard]] bool parse_decimal(std::string_view text, std::size_t offset, std::size_t count,
                                 int& out) noexcept {
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (!is_digit(c)) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

// Writes `value` zero padded to at least `digits` decimal digits.
void append_fixed(std::string& out, std::uint64_t value, std::size_t digits) {
  char buffer[24] = {};
  std::size_t count = 0;
  do {
    buffer[count] = static_cast<char>('0' + static_cast<int>(value % 10u));
    ++count;
    value /= 10u;
  } while (value != 0 && count < sizeof(buffer));
  while (count < digits && count < sizeof(buffer)) {
    buffer[count] = '0';
    ++count;
  }
  while (count > 0) {
    --count;
    out.push_back(buffer[count]);
  }
}

// Expanded ISO 8601 year: a leading '-' for years before year zero, then at
// least four digits. The magnitude is computed without ever negating the value,
// so std::int64_t minimum is safe.
void append_year(std::string& out, std::int64_t year) {
  if (year < 0) {
    out.push_back('-');
    const auto magnitude = static_cast<std::uint64_t>(-(year + 1)) + 1u;
    append_fixed(out, magnitude, 4);
    return;
  }
  append_fixed(out, static_cast<std::uint64_t>(year), 4);
}

[[nodiscard]] std::string format_utc(std::int64_t nanos, std::size_t fraction_digits) {
  // Floor split into whole seconds and a non negative nanosecond fraction. The
  // plain '/' and '%' operators truncate toward zero, so negative instants need
  // the correction below; no negation of a possibly minimal value happens here.
  std::int64_t seconds = nanos / kNanosPerSecond;
  std::int64_t fraction = nanos % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    --seconds;
  }
  std::int64_t days = seconds / kSecondsPerDay;
  std::int64_t second_of_day = seconds % kSecondsPerDay;
  if (second_of_day < 0) {
    second_of_day += kSecondsPerDay;
    --days;
  }

  std::int64_t year = 0;
  int month = 0;
  int day = 0;
  civil_from_days(days, year, month, day);

  const auto hour = static_cast<std::uint64_t>(second_of_day / kSecondsPerHour);
  const auto minute = static_cast<std::uint64_t>((second_of_day % kSecondsPerHour) / kSecondsPerMinute);
  const auto second = static_cast<std::uint64_t>(second_of_day % kSecondsPerMinute);

  std::string out;
  out.reserve(35);
  append_year(out, year);
  out.push_back('-');
  append_fixed(out, static_cast<std::uint64_t>(month), 2);
  out.push_back('-');
  append_fixed(out, static_cast<std::uint64_t>(day), 2);
  out.push_back('T');
  append_fixed(out, hour, 2);
  out.push_back(':');
  append_fixed(out, minute, 2);
  out.push_back(':');
  append_fixed(out, second, 2);
  if (fraction_digits > 0) {
    std::uint64_t scale = 1;
    for (std::size_t i = 0; i < fraction_digits; ++i) {
      scale *= 10u;
    }
    const auto divisor = static_cast<std::uint64_t>(kNanosPerSecond) / scale;
    out.push_back('.');
    append_fixed(out, static_cast<std::uint64_t>(fraction) / divisor, fraction_digits);
  }
  out.push_back('Z');
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// TimePoint formatting
// ---------------------------------------------------------------------------

std::string TimePoint::to_rfc3339() const { return format_utc(nanos_, 0); }

std::string TimePoint::to_rfc3339_millis() const { return format_utc(nanos_, 3); }

// ---------------------------------------------------------------------------
// Duration formatting
// ---------------------------------------------------------------------------

std::string Duration::to_iso8601() const {
  const bool negative = nanos_ < 0;
  // Magnitude without negation so that std::int64_t minimum cannot overflow.
  const std::uint64_t magnitude =
      negative ? (static_cast<std::uint64_t>(-(nanos_ + 1)) + 1u)
               : static_cast<std::uint64_t>(nanos_);
  const auto total_seconds = magnitude / static_cast<std::uint64_t>(kNanosPerSecond);
  const std::uint64_t days = total_seconds / static_cast<std::uint64_t>(kSecondsPerDay);
  const std::uint64_t hours =
      (total_seconds % static_cast<std::uint64_t>(kSecondsPerDay)) / static_cast<std::uint64_t>(kSecondsPerHour);
  const std::uint64_t minutes =
      (total_seconds % static_cast<std::uint64_t>(kSecondsPerHour)) / static_cast<std::uint64_t>(kSecondsPerMinute);
  const std::uint64_t seconds = total_seconds % static_cast<std::uint64_t>(kSecondsPerMinute);

  // Sub-second remainders are truncated toward zero, so a magnitude below one
  // second formats as an unsigned "PT0S" rather than a signed zero.
  const bool signed_output = negative && total_seconds != 0;

  std::string out;
  out.reserve(32);
  if (signed_output) {
    out.push_back('-');
  }
  out.push_back('P');
  if (days != 0) {
    out.append(std::to_string(days));
    out.push_back('D');
  }
  const bool has_time_part = hours != 0 || minutes != 0 || seconds != 0 || days == 0;
  if (has_time_part) {
    out.push_back('T');
    if (hours != 0) {
      out.append(std::to_string(hours));
      out.push_back('H');
    }
    if (minutes != 0) {
      out.append(std::to_string(minutes));
      out.push_back('M');
    }
    if (seconds != 0 || (hours == 0 && minutes == 0)) {
      out.append(std::to_string(seconds));
      out.push_back('S');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// RFC 3339 parsing
// ---------------------------------------------------------------------------

Result<TimePoint> parse_rfc3339(std::string_view text) {
  const auto malformed = [](std::string_view what, std::string_view input) -> Error {
    constexpr std::size_t kMaxEcho = 64;
    return Error{ErrorCode::MalformedTimestamp, std::string(what), std::string(input.substr(0, kMaxEcho))};
  };

  if (text.size() > kMaxTimestampLength) {
    return malformed("timestamp is longer than 64 characters", text);
  }
  // "YYYY-MM-DDTHH:MM:SSZ" is the shortest accepted form.
  if (text.size() < 20) {
    return malformed("timestamp is shorter than YYYY-MM-DDTHH:MM:SSZ", text);
  }
  if (text[4] != '-' || text[7] != '-') {
    return malformed("date must be YYYY-MM-DD", text);
  }
  const char date_time_separator = text[10];
  if (date_time_separator != 'T' && date_time_separator != 't' && date_time_separator != ' ') {
    return malformed("date and time must be separated by 'T', 't' or a space", text);
  }
  if (text[13] != ':' || text[16] != ':') {
    return malformed("time must be HH:MM:SS", text);
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_decimal(text, 0, 4, year)) {
    return malformed("year must be four decimal digits", text);
  }
  if (!parse_decimal(text, 5, 2, month)) {
    return malformed("month must be two decimal digits", text);
  }
  if (!parse_decimal(text, 8, 2, day)) {
    return malformed("day must be two decimal digits", text);
  }
  if (!parse_decimal(text, 11, 2, hour)) {
    return malformed("hour must be two decimal digits", text);
  }
  if (!parse_decimal(text, 14, 2, minute)) {
    return malformed("minute must be two decimal digits", text);
  }
  if (!parse_decimal(text, 17, 2, second)) {
    return malformed("second must be two decimal digits", text);
  }

  if (month < 1 || month > 12) {
    return malformed("month must be 01..12", text);
  }
  if (day < 1 || day > days_in_month(static_cast<std::int64_t>(year), month)) {
    return malformed("day is out of range for the month", text);
  }
  if (hour > 23) {
    return malformed("hour must be 00..23", text);
  }
  if (minute > 59) {
    return malformed("minute must be 00..59", text);
  }
  if (second > 59) {
    return malformed("second must be 00..59; leap seconds are not accepted", text);
  }

  std::size_t index = 19;
  std::int64_t fraction_nanos = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    std::size_t digits = 0;
    while (index < text.size() && is_digit(text[index])) {
      if (digits < kMaxFractionDigits) {
        fraction_nanos = fraction_nanos * 10 + static_cast<std::int64_t>(text[index] - '0');
      }
      ++digits;
      ++index;
    }
    if (digits == 0) {
      return malformed("fractional seconds need at least one digit", text);
    }
    if (digits > kMaxFractionDigits) {
      return malformed("fractional seconds must have at most nine digits", text);
    }
    for (std::size_t i = digits; i < kMaxFractionDigits; ++i) {
      fraction_nanos *= 10;
    }
  }

  if (index >= text.size()) {
    return malformed("timestamp must end with 'Z' or an explicit +HH:MM/-HH:MM offset", text);
  }
  std::int64_t offset_seconds = 0;
  const char zone = text[index];
  if (zone == 'Z' || zone == 'z') {
    ++index;
  } else if (zone == '+' || zone == '-') {
    if (text.size() - index < 6) {
      return malformed("offset must be written as +HH:MM or -HH:MM", text);
    }
    if (text[index + 3] != ':') {
      return malformed("offset must be written as +HH:MM or -HH:MM", text);
    }
    int offset_hour = 0;
    int offset_minute = 0;
    if (!parse_decimal(text, index + 1, 2, offset_hour) ||
        !parse_decimal(text, index + 4, 2, offset_minute)) {
      return malformed("offset must be written as +HH:MM or -HH:MM", text);
    }
    if (offset_hour > 23) {
      return malformed("offset hours must be 00..23", text);
    }
    if (offset_minute > 59) {
      return malformed("offset minutes must be 00..59", text);
    }
    offset_seconds = static_cast<std::int64_t>(offset_hour) * kSecondsPerHour +
                     static_cast<std::int64_t>(offset_minute) * kSecondsPerMinute;
    if (zone == '-') {
      offset_seconds = -offset_seconds;
    }
    index += 6;
  } else {
    return malformed("timestamp must end with 'Z' or an explicit +HH:MM/-HH:MM offset", text);
  }
  if (index != text.size()) {
    return malformed("trailing characters after the timestamp", text);
  }

  const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), month, day);
  const std::int64_t local_seconds = days * kSecondsPerDay + static_cast<std::int64_t>(hour) * kSecondsPerHour +
                                     static_cast<std::int64_t>(minute) * kSecondsPerMinute +
                                     static_cast<std::int64_t>(second);
  const std::int64_t utc_seconds = local_seconds - offset_seconds;
  if (utc_seconds > kMaxRepresentableSeconds || utc_seconds < kMinRepresentableSeconds - 1) {
    return malformed("timestamp is outside the representable nanosecond range", text);
  }
  if (utc_seconds == kMaxRepresentableSeconds && fraction_nanos > kMaxFractionAtUpperBound) {
    return malformed("timestamp is past the last representable nanosecond", text);
  }
  if (utc_seconds == kMinRepresentableSeconds - 1 && fraction_nanos < kMinFractionAtLowerBound) {
    return malformed("timestamp is before the first representable nanosecond", text);
  }
  // The second just below the window floor cannot be scaled on its own: the
  // product would overflow, so its fraction is applied relative to the floor.
  const std::int64_t utc_nanos =
      (utc_seconds == kMinRepresentableSeconds - 1)
          ? (kMinRepresentableSeconds * kNanosPerSecond + (fraction_nanos - kNanosPerSecond))
          : (utc_seconds * kNanosPerSecond + fraction_nanos);
  return TimePoint::from_nanos(utc_nanos);
}

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

TimePoint SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
  return TimePoint::from_nanos(static_cast<std::int64_t>(nanos.count()));
}

}  // namespace fel
