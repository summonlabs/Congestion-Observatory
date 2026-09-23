// Congestion Observatory - minimal, dependency free test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The framework deliberately provides no timeouts: a test either completes or the process is
// stopped by the operator. Every failure carries an expression, a file and a line.

#ifndef CONGESTION_TESTS_TEST_FRAMEWORK_HPP
#define CONGESTION_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "congestion/congestion.hpp"

namespace cotest {

class Context;

using TestFunction = void (*)(Context&);

struct TestCase {
  std::string suite{};
  std::string name{};
  TestFunction function{nullptr};
};

[[nodiscard]] std::vector<TestCase>& registry();
[[nodiscard]] std::size_t register_test(const char* suite, const char* name, TestFunction function);

// Human readable rendering of a compared value. Types with a stream operator are rendered
// directly; domain types get explicit overloads below; anything else reports a placeholder rather
// than failing to compile, so that a new compared type never breaks the suite.
template <class T>
std::string describe(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& candidate) { stream << candidate; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(std::string_view value) { return std::string(value); }
inline std::string describe(const std::string& value) { return value; }
inline std::string describe(const char* value) { return value == nullptr ? "(null)" : value; }

// Scoped enumerations and domain identities have no stream operator: render them through the
// library's own to_string functions so that failures are readable.
inline std::string describe(congestion::Verdict value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Severity value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Mechanism value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Freshness value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Agreement value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Support value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::Completeness value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::FenceDecision value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::ErrorCode value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::EpisodeState value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::EpisodeUpdateKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::LocalizationOutcome value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::LoadOutcome value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::MergeDecisionCode value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::SubjectKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::EvidenceKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::EvidenceRole value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::EpisodeTransitionKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::CausalEdgeKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::NodeKind value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::ObservationUnit value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::ValueSemantics value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::ClockDomain value) {
  return std::string(congestion::to_string(value));
}
inline std::string describe(congestion::GenerationOrder value) {
  switch (value) {
    case congestion::GenerationOrder::kOlder: return "older";
    case congestion::GenerationOrder::kEqual: return "equal";
    case congestion::GenerationOrder::kNewer: return "newer";
    case congestion::GenerationOrder::kIncomparable: return "incomparable";
  }
  return "unknown";
}
inline std::string describe(const congestion::Name& value) { return value.str(); }
inline std::string describe(const congestion::EvidenceId& value) { return value.str(); }
inline std::string describe(const congestion::EpisodeId& value) { return value.str(); }
inline std::string describe(const congestion::GroupId& value) { return value.str(); }
inline std::string describe(const congestion::EvidenceSubject& value) { return value.str(); }
inline std::string describe(const congestion::Timestamp& value) { return value.to_iso8601(); }
inline std::string describe(const congestion::Duration& value) { return value.to_string(); }

// Containers of comparable values are rendered element by element.
template <class T>
std::string describe(const std::vector<T>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += describe(values[i]);
  }
  out += "]";
  return out;
}

class Context {
 public:
  Context(std::string suite, std::string name) : suite_(std::move(suite)), name_(std::move(name)) {}

  void fail(const std::string& message, const char* file, int line) {
    ++failures_;
    std::ostringstream stream;
    stream << "    FAIL " << file << ":" << line << ": " << message << "\n";
    log_ += stream.str();
  }

  void note(const std::string& message) {
    std::ostringstream stream;
    stream << "    note: " << message << "\n";
    log_ += stream.str();
  }

  [[nodiscard]] bool failed() const noexcept { return failures_ != 0; }
  [[nodiscard]] std::size_t failures() const noexcept { return failures_; }
  [[nodiscard]] const std::string& log() const noexcept { return log_; }
  [[nodiscard]] const std::string& suite() const noexcept { return suite_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  std::string suite_{};
  std::string name_{};
  std::size_t failures_{0};
  std::string log_{};
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function) {
    (void)register_test(suite, name, function);
  }
};

// Runs the suites selected by the command line. Returns the process exit code.
int run_all(int argc, char** argv);

}  // namespace cotest

// Declares and registers a test case. The generated function name is unique per suite/name pair.
#define CO_TEST(suite_name, test_name)                                                        \
  static void co_test_##suite_name##_##test_name(::cotest::Context& co_ctx);                  \
  static const ::cotest::Registrar co_registrar_##suite_name##_##test_name(                   \
      #suite_name, #test_name, &co_test_##suite_name##_##test_name);                          \
  static void co_test_##suite_name##_##test_name(::cotest::Context& co_ctx)

#define CO_EXPECT(condition)                                                                  \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      co_ctx.fail(std::string("expected: ") + #condition, __FILE__, __LINE__);                 \
    }                                                                                         \
  } while (false)

#define CO_EXPECT_EQ(actual, expected)                                                        \
  do {                                                                                        \
    const auto& co_actual_value = (actual);                                                    \
    const auto& co_expected_value = (expected);                                                \
    if (!(co_actual_value == co_expected_value)) {                                             \
      co_ctx.fail(std::string("expected ") + #actual + " == " + #expected + " but got " +      \
                      ::cotest::describe(co_actual_value) + " != " +                           \
                      ::cotest::describe(co_expected_value),                                  \
                  __FILE__, __LINE__);                                                         \
    }                                                                                         \
  } while (false)

#define CO_EXPECT_NE(actual, expected)                                                        \
  do {                                                                                        \
    const auto& co_actual_value = (actual);                                                    \
    const auto& co_expected_value = (expected);                                                \
    if (co_actual_value == co_expected_value) {                                                \
      co_ctx.fail(std::string("expected ") + #actual + " != " + #expected, __FILE__, __LINE__);\
    }                                                                                         \
  } while (false)

#define CO_EXPECT_OK(expression)                                                              \
  do {                                                                                        \
    const auto& co_result_value = (expression);                                               \
    if (!co_result_value.ok()) {                                                              \
      co_ctx.fail(std::string("expected success from ") + #expression + " but got " +         \
                      ::std::string(::congestion::to_string(co_result_value.error().code())) + \
                      ": " + co_result_value.error().describe(),                              \
                  __FILE__, __LINE__);                                                        \
    }                                                                                         \
  } while (false)

// Fails and stops the test immediately. Used before dereferencing a Result, so that a failed
// expectation can never turn into an exception thrown from value().
#define CO_REQUIRE(condition)                                                                   do {                                                                                            if (!(condition)) {                                                                             co_ctx.fail(std::string("required: ") + #condition, __FILE__, __LINE__);                       return;                                                                                     }                                                                                           } while (false)

#define CO_REQUIRE_OK(expression)                                                               do {                                                                                            const auto& co_required_value = (expression);                                                 if (!co_required_value.ok()) {                                                                  co_ctx.fail(std::string("required success from ") + #expression + " but got " +                               ::std::string(::congestion::to_string(co_required_value.error().code())) +                      ": " + co_required_value.error().describe(),                                              __FILE__, __LINE__);                                                              return;                                                                                     }                                                                                           } while (false)

#define CO_EXPECT_ERR(expression, expected_code)                                              \
  do {                                                                                        \
    const auto& co_result_value = (expression);                                               \
    if (co_result_value.ok()) {                                                               \
      co_ctx.fail(std::string("expected failure from ") + #expression, __FILE__, __LINE__);   \
    } else if (co_result_value.error().code() != (expected_code)) {                           \
      co_ctx.fail(std::string("expected ") + #expression + " to fail with " +                 \
                      #expected_code + " but got " +                                          \
                      ::std::string(::congestion::to_string(co_result_value.error().code())), \
                  __FILE__, __LINE__);                                                        \
    }                                                                                         \
  } while (false)

#define CO_EXPECT_STATUS_ERR(expression, expected_code)                                       \
  do {                                                                                        \
    const auto co_status_value = (expression);                                                \
    if (co_status_value.ok()) {                                                               \
      co_ctx.fail(std::string("expected failure from ") + #expression, __FILE__, __LINE__);   \
    } else if (co_status_value.code() != (expected_code)) {                                   \
      co_ctx.fail(std::string("expected ") + #expression + " to fail with " +                 \
                      #expected_code + " but got " +                                          \
                      ::std::string(::congestion::to_string(co_status_value.code())),         \
                  __FILE__, __LINE__);                                                        \
    }                                                                                         \
  } while (false)

#endif  // CONGESTION_TESTS_TEST_FRAMEWORK_HPP
