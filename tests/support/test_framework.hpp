// Fabric Efficiency Ledger - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace feltest {

// Thrown by a failed assertion. FEL_REQUIRE and FEL_ASSERT_OK abort the current
// test case; FEL_EXPECT records the failure and continues. No assertion ever
// depends on a timeout, and the runner has no timeout of its own.
class AssertionFailure : public std::exception {
 public:
  explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

class Registry {
 public:
  static Registry& instance();

  void add(std::string suite, std::string name, std::function<void()> body);
  // Returns the process exit code: 0 when every selected case passed.
  int run(const std::string& suite_filter, const std::string& name_filter) const;

  [[nodiscard]] std::size_t size() const noexcept { return cases_.size(); }
  [[nodiscard]] std::vector<std::string> suites() const;

 private:
  std::vector<TestCase> cases_;
};

struct Registration {
  Registration(const char* suite, const char* name, std::function<void()> body) {
    Registry::instance().add(suite, name, std::move(body));
  }
};

void record_failure(const char* file, int line, std::string message);

// The checks are free functions rather than macro bodies so that a constant
// condition (a perfectly ordinary thing in a unit test) does not trip the
// compiler's "conditional expression is constant" diagnostic.
void check(bool condition, const char* file, int line, const std::string& text);
void require(bool condition, const char* file, int line, const std::string& text);

template <class A, class B>
void check_equal(const A& actual, const B& expected, const char* file, int line,
                 const char* actual_text, const char* expected_text) {
  if (!(actual == expected)) {
    record_failure(file, line,
                   std::string("expected ") + actual_text + " == " + expected_text);
  }
}

}  // namespace feltest

#define FEL_TEST(suite_name, case_name)                                            \
  static void fel_test_body_##suite_name##_##case_name();                          \
  static const ::feltest::Registration fel_test_reg_##suite_name##_##case_name(    \
      #suite_name, #case_name, &fel_test_body_##suite_name##_##case_name);         \
  static void fel_test_body_##suite_name##_##case_name()

#define FEL_EXPECT(condition)                                                        \
  ::feltest::check(static_cast<bool>(condition), __FILE__, __LINE__, #condition)

#define FEL_REQUIRE(condition)                                                       \
  ::feltest::require(static_cast<bool>(condition), __FILE__, __LINE__, #condition)

// Both operands are copied: an operand that returns a reference into a
// temporary (for example a Result accessor) would otherwise dangle for the
// duration of the comparison.
#define FEL_EXPECT_EQ(actual, expected)                                              \
  do {                                                                               \
    const auto fel_actual_ = (actual);                                               \
    const auto fel_expected_ = (expected);                                           \
    ::feltest::check_equal(fel_actual_, fel_expected_, __FILE__, __LINE__, #actual,  \
                           #expected);                                               \
  } while (false)

namespace feltest {

// Renders the two values of a failed FEL_EXPECT_EQ when they are streamable.
template <class A, class B>
[[nodiscard]] std::string describe_values(const A& actual, const B& expected) {
  std::string text = " values differ";
  (void)actual;
  (void)expected;
  return text;
}

[[nodiscard]] inline std::string describe_mismatch(const char* actual, const char* expected) {
  return std::string("expected ") + actual + " == " + expected;
}

}  // namespace feltest

#define FEL_ASSERT_OK(expression)                                                    \
  do {                                                                               \
    auto fel_result_ = (expression);                                                 \
    if (!fel_result_.has_value()) {                                                  \
      ::feltest::record_failure(__FILE__, __LINE__,                                  \
                                std::string(#expression) + " failed: " +             \
                                    fel_result_.error().to_string());                \
    }                                                                                \
  } while (false)

#define FEL_ASSERT_ERR(expression, expected_code)                                    \
  do {                                                                               \
    auto fel_result_ = (expression);                                                 \
    if (fel_result_.has_value()) {                                                   \
      ::feltest::record_failure(__FILE__, __LINE__,                                  \
                                std::string(#expression) + " unexpectedly succeeded"); \
    } else if (fel_result_.error().code != (expected_code)) {                        \
      ::feltest::record_failure(__FILE__, __LINE__,                                  \
                                std::string(#expression) + " returned " +            \
                                    std::string(::fel::error_code_name(              \
                                        fel_result_.error().code)) +                 \
                                    " instead of " + #expected_code);                \
    }                                                                                \
  } while (false)
