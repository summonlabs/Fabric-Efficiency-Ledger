// Fabric Efficiency Ledger - checked arithmetic and aggregate cells.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <limits>

#include "fel/checked.hpp"
#include "fel/measure.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, add_sub_mul_boundaries) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  FEL_EXPECT_EQ(*add_checked(1, 2), std::uint64_t{3});
  FEL_EXPECT(!add_checked(maximum, 1).has_value());
  FEL_EXPECT_EQ(*add_checked(maximum, 0), maximum);
  FEL_EXPECT_EQ(*sub_checked(5, 5), std::uint64_t{0});
  FEL_EXPECT(!sub_checked(0, 1).has_value());
  FEL_EXPECT_EQ(*sub_checked(maximum, maximum), std::uint64_t{0});
  FEL_EXPECT_EQ(*mul_checked(0, maximum), std::uint64_t{0});
  FEL_EXPECT_EQ(*mul_checked(maximum, 1), maximum);
  FEL_EXPECT(!mul_checked(maximum, 2).has_value());
}

FEL_TEST(unit, signed_checked_arithmetic_boundaries) {
  const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  const std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  FEL_EXPECT_EQ(*add_checked_i64(1, -1), std::int64_t{0});
  FEL_EXPECT(!add_checked_i64(maximum, 1).has_value());
  FEL_EXPECT(!add_checked_i64(minimum, -1).has_value());
  FEL_EXPECT(!mul_checked_i64(maximum, 2).has_value());
  FEL_EXPECT_EQ(*mul_checked_i64(-3, 4), std::int64_t{-12});
}

FEL_TEST(unit, checked_sum_reports_overflow) {
  CheckedSum sum;
  FEL_ASSERT_OK(sum.add(10));
  FEL_ASSERT_OK(sum.add(32));
  FEL_EXPECT_EQ(sum.total(), std::uint64_t{42});
  FEL_EXPECT_EQ(sum.additions(), std::uint64_t{2});
  FEL_EXPECT(!sum.empty());
  FEL_ASSERT_OK(sum.add(std::numeric_limits<std::uint64_t>::max() - 42));
  FEL_ASSERT_ERR(sum.add(1), ErrorCode::ArithmeticOverflow);
}

FEL_TEST(unit, aggregate_cell_coverage_transitions) {
  AggregateCell cell;
  cell.kind = MeasureKind::WireBytes;
  FEL_EXPECT_EQ(cell.coverage(), Coverage::Known);
  FEL_ASSERT_OK(cell.add_known(100));
  FEL_EXPECT_EQ(cell.coverage(), Coverage::Known);
  FEL_ASSERT_OK(cell.add_unknown());
  FEL_EXPECT_EQ(cell.coverage(), Coverage::Partial);
  FEL_REQUIRE(cell.value_if_known().has_value() == false);

  AggregateCell unknown_only;
  unknown_only.kind = MeasureKind::WireBytes;
  FEL_ASSERT_OK(unknown_only.add_unknown());
  FEL_EXPECT_EQ(unknown_only.coverage(), Coverage::Unknown);
  FEL_EXPECT(!unknown_only.has_value());
}

FEL_TEST(unit, aggregate_cell_merge_refuses_incompatible_kinds) {
  AggregateCell bytes;
  bytes.kind = MeasureKind::WireBytes;
  FEL_ASSERT_OK(bytes.add_known(10));
  AggregateCell nanos;
  nanos.kind = MeasureKind::PortOccupancyNanos;
  FEL_ASSERT_OK(nanos.add_known(20));
  FEL_ASSERT_ERR(bytes.merge(nanos), ErrorCode::IncompatibleMeasureKind);
  FEL_EXPECT_EQ(bytes.known_total, std::uint64_t{10});
}

FEL_TEST(unit, aggregate_cell_merge_and_overflow) {
  AggregateCell left;
  left.kind = MeasureKind::PayloadBytes;
  FEL_ASSERT_OK(left.add_known(7));
  AggregateCell right;
  right.kind = MeasureKind::PayloadBytes;
  FEL_ASSERT_OK(right.add_known(35));
  FEL_ASSERT_OK(right.add_unknown());
  FEL_ASSERT_OK(left.merge(right));
  FEL_EXPECT_EQ(left.known_total, std::uint64_t{42});
  FEL_EXPECT_EQ(left.known_contributions, std::uint64_t{2});
  FEL_EXPECT_EQ(left.unknown_contributions, std::uint64_t{1});
  FEL_EXPECT_EQ(left.coverage(), Coverage::Partial);

  AggregateCell huge;
  huge.kind = MeasureKind::PayloadBytes;
  FEL_ASSERT_OK(huge.add_known(std::numeric_limits<std::uint64_t>::max()));
  FEL_ASSERT_ERR(huge.add_known(1), ErrorCode::ArithmeticOverflow);
}
