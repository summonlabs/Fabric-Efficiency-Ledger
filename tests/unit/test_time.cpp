// Fabric Efficiency Ledger - time parsing, formatting and arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fel/time.hpp"
#include "support/test_framework.hpp"

using namespace fel;

namespace {

[[nodiscard]] bool accepts(const std::string& text) {
  return parse_rfc3339(text).has_value();
}

}  // namespace

FEL_TEST(unit, rfc3339_accepts_the_documented_forms) {
  FEL_REQUIRE(accepts("2024-01-02T03:04:05Z"));
  FEL_REQUIRE(accepts("2024-01-02t03:04:05z"));
  FEL_REQUIRE(accepts("2024-01-02 03:04:05Z"));
  FEL_REQUIRE(accepts("2024-01-02T03:04:05.123Z"));
  FEL_REQUIRE(accepts("2024-01-02T03:04:05.123456789Z"));
  FEL_REQUIRE(accepts("2024-01-02T03:04:05+02:00"));
  FEL_REQUIRE(accepts("2024-01-02T03:04:05-05:30"));
  FEL_REQUIRE(accepts("2024-02-29T00:00:00Z"));
}

FEL_TEST(unit, rfc3339_rejects_out_of_range_and_malformed_input) {
  const char* rejected[] = {"2024-13-01T00:00:00Z", "2024-02-30T00:00:00Z",
                            "2024-01-02T24:00:00Z", "2024-01-02T00:60:00Z",
                            "2024-01-02T00:00:60Z", "", "garbage", "2023-02-29T00:00:00Z",
                            "2024-00-02T00:00:00Z", "2024-01-00T00:00:00Z"};
  for (const char* text : rejected) {
    auto parsed = parse_rfc3339(text);
    FEL_EXPECT(!parsed.has_value());
    if (!parsed.has_value()) {
      FEL_EXPECT_EQ(parsed.error().code, ErrorCode::MalformedTimestamp);
    }
  }
  FEL_EXPECT(!accepts(std::string(100, 'a')));
  FEL_EXPECT(!accepts("2024-01-02T03:04:05Zextra"));
}

FEL_TEST(unit, rfc3339_offsets_are_applied) {
  auto utc = parse_rfc3339("2024-01-02T03:04:05Z");
  auto plus_two = parse_rfc3339("2024-01-02T05:04:05+02:00");
  FEL_REQUIRE(utc.has_value());
  FEL_REQUIRE(plus_two.has_value());
  FEL_EXPECT_EQ(utc.value(), plus_two.value());
}

FEL_TEST(unit, timestamp_round_trip_including_before_the_epoch) {
  const std::int64_t instants[] = {0, 1, 1700000000, -1, -86400, 4102444800LL};
  for (const std::int64_t seconds : instants) {
    const TimePoint instant = TimePoint::from_seconds(seconds);
    auto reparsed = parse_rfc3339(instant.to_rfc3339());
    FEL_REQUIRE(reparsed.has_value());
    FEL_EXPECT_EQ(reparsed.value(), instant);
  }
}

FEL_TEST(unit, duration_arithmetic_and_formatting) {
  FEL_EXPECT_EQ(Duration::from_seconds(90).millis(), std::int64_t{90000});
  FEL_EXPECT_EQ((Duration::from_seconds(60) - Duration::from_seconds(90)).seconds(),
                std::int64_t{-30});
  FEL_EXPECT((Duration::from_seconds(60) - Duration::from_seconds(90)).is_negative());
  FEL_EXPECT(!Duration{}.is_negative());
  FEL_EXPECT(Duration{}.is_zero());
  FEL_EXPECT(!Duration::from_hours(1).to_iso8601().empty());
  FEL_EXPECT_EQ(Duration::from_seconds(90).to_iso8601(), std::string("PT1M30S"));
}

FEL_TEST(unit, time_arithmetic_is_exact_integer_nanoseconds) {
  const TimePoint start = TimePoint::from_seconds(1700000000);
  const TimePoint later = start + Duration::from_nanos(1);
  FEL_EXPECT_EQ((later - start).nanos(), std::int64_t{1});
  FEL_EXPECT_EQ((start - later).nanos(), std::int64_t{-1});
  FEL_EXPECT(start < later);

  ManualClock clock(start);
  FEL_EXPECT_EQ(clock.now(), start);
  clock.advance(Duration::from_minutes(1));
  FEL_EXPECT_EQ(clock.now(), start + Duration::from_minutes(1));
  clock.set(later);
  FEL_EXPECT_EQ(clock.now(), later);
}
