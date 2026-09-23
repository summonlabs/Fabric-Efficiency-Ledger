// Fabric Efficiency Ledger - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "support/test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace feltest {
namespace {

thread_local int g_failures_in_case = 0;

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string suite, std::string name, std::function<void()> body) {
  TestCase test_case;
  test_case.suite = std::move(suite);
  test_case.name = std::move(name);
  test_case.body = std::move(body);
  cases_.push_back(std::move(test_case));
}

std::vector<std::string> Registry::suites() const {
  std::set<std::string> unique;
  for (const auto& test_case : cases_) {
    unique.insert(test_case.suite);
  }
  return std::vector<std::string>(unique.begin(), unique.end());
}

void record_failure(const char* file, int line, std::string message) {
  ++g_failures_in_case;
  std::cout << "    FAIL " << file << ":" << line << ": " << message << "\n";
}

void check(bool condition, const char* file, int line, const std::string& text) {
  if (!condition) {
    record_failure(file, line, "expected: " + text);
  }
}

void require(bool condition, const char* file, int line, const std::string& text) {
  if (!condition) {
    record_failure(file, line, "required: " + text);
    throw AssertionFailure(file + std::string(":") + std::to_string(line) + ": " + text);
  }
}

int Registry::run(const std::string& suite_filter, const std::string& name_filter) const {
  std::size_t executed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failed_names;
  for (const auto& test_case : cases_) {
    if (!suite_filter.empty() && test_case.suite != suite_filter) {
      continue;
    }
    if (!name_filter.empty() && test_case.name.find(name_filter) == std::string::npos) {
      continue;
    }
    ++executed;
    g_failures_in_case = 0;
    std::cout << "[ RUN  ] " << test_case.suite << "." << test_case.name << "\n";
    try {
      test_case.body();
    } catch (const AssertionFailure& failure) {
      ++g_failures_in_case;
      std::cout << "    ABORT: " << failure.what() << "\n";
    } catch (const std::exception& error) {
      ++g_failures_in_case;
      std::cout << "    EXCEPTION: " << error.what() << "\n";
    } catch (...) {
      ++g_failures_in_case;
      std::cout << "    EXCEPTION: unknown\n";
    }
    if (g_failures_in_case == 0) {
      std::cout << "[  OK  ] " << test_case.suite << "." << test_case.name << "\n";
    } else {
      ++failed;
      failed_names.push_back(test_case.suite + "." + test_case.name);
      std::cout << "[ FAIL ] " << test_case.suite << "." << test_case.name << "\n";
    }
  }
  std::cout << "\n"
            << "cases: " << executed << "  passed: " << (executed - failed) << "  failed: " << failed
            << "\n";
  for (const auto& name : failed_names) {
    std::cout << "failed: " << name << "\n";
  }
  std::cout.flush();
  return failed == 0 ? 0 : 1;
}

}  // namespace feltest

int main(int argc, char** argv) {
  std::string suite_filter;
  std::string name_filter;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--suite" && i + 1 < argc) {
      suite_filter = argv[++i];
    } else if (argument == "--filter" && i + 1 < argc) {
      name_filter = argv[++i];
    } else if (argument == "--list") {
      for (const auto& suite : feltest::Registry::instance().suites()) {
        std::cout << suite << "\n";
      }
      return 0;
    } else {
      std::cout << "usage: " << argv[0] << " [--suite NAME] [--filter SUBSTRING] [--list]\n";
      return 2;
    }
  }
  return feltest::Registry::instance().run(suite_filter, name_filter);
}
