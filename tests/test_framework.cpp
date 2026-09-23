// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"

#include <algorithm>
#include <cstring>
#include <exception>

namespace cotest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::size_t register_test(const char* suite, const char* name, TestFunction function) {
  registry().push_back(TestCase{suite, name, function});
  return registry().size();
}

int run_all(int argc, char** argv) {
  std::string suite_filter;
  std::string name_filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--suite" && i + 1 < argc) {
      suite_filter = argv[++i];
    } else if (argument == "--filter" && i + 1 < argc) {
      name_filter = argv[++i];
    } else {
      std::cerr << "unknown argument: " << argument << "\n";
      return 2;
    }
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : registry()) {
    if (!suite_filter.empty() && test.suite != suite_filter) {
      continue;
    }
    if (!name_filter.empty() && test.name.find(name_filter) == std::string::npos) {
      continue;
    }
    selected.push_back(test);
  }
  std::sort(selected.begin(), selected.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  if (list_only) {
    for (const TestCase& test : selected) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return 0;
  }
  if (selected.empty()) {
    std::cerr << "no tests selected\n";
    return 2;
  }

  std::size_t failures = 0;
  std::string current_suite;
  for (const TestCase& test : selected) {
    if (test.suite != current_suite) {
      current_suite = test.suite;
      std::cout << "[" << current_suite << "]\n";
    }
    Context context(test.suite, test.name);
    try {
      test.function(context);
    } catch (const std::exception& error) {
      // A test that throws is a failed test, not a lost suite.
      context.fail(std::string("threw an exception: ") + error.what(), __FILE__, __LINE__);
    } catch (...) {
      context.fail("threw a non standard exception", __FILE__, __LINE__);
    }
    if (context.failed()) {
      ++failures;
      std::cout << "  FAILED " << test.name << " (" << context.failures() << " check(s))\n";
      std::cout << context.log();
    } else {
      std::cout << "  ok     " << test.name << "\n";
      if (!context.log().empty()) {
        std::cout << context.log();
      }
    }
  }
  std::cout << (failures == 0 ? "PASS" : "FAIL") << " " << selected.size() << " test(s), "
            << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}

}  // namespace cotest

int main(int argc, char** argv) { return cotest::run_all(argc, argv); }
