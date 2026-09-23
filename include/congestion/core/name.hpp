// Congestion Observatory - canonical bounded names used for domain identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_NAME_HPP
#define CONGESTION_CORE_NAME_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "congestion/core/result.hpp"

namespace congestion {

// A Name is the canonical textual identity of a domain object. Names are what make persisted
// identities stable and human-explainable: the same textual name always denotes the same object,
// independent of host, locale, run or pointer values.
//
// Canonical form rules (enforced by parse()):
//   * ASCII only, no control characters, no whitespace;
//   * characters limited to [A-Za-z0-9] plus '-', '_', '.', ':', '/', '@', '#', '|', '+', '=', ','
//     -- enough for switch/port/queue/flow/topology naming without opening an encoding ambiguity;
//   * length in [1, kMaxLength];
//   * no leading or trailing separator and no empty path segment ("//");
//   * case is preserved but never normalised: "Leaf1" and "leaf1" are distinct names.
class Name {
 public:
  static constexpr std::size_t kMaxLength = 96;

  Name() = default;

  // Parses and validates a name. Returns kInvalidArgument when the text is not canonical.
  [[nodiscard]] static Result<Name> parse(std::string_view text);

  // Creates a Name from a literal the caller has already verified (or a compile-time constant).
  // In debug builds the input is validated and a failed precondition is reported through
  // unchecked_failure_count() so tests can prove no invalid literal slipped in.
  [[nodiscard]] static Name unchecked(std::string_view text) noexcept;

  [[nodiscard]] bool valid() const noexcept { return !value_.empty(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  friend bool operator==(const Name& lhs, const Name& rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend bool operator!=(const Name& lhs, const Name& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const Name& lhs, const Name& rhs) noexcept { return lhs.value_ < rhs.value_; }
  friend bool operator>(const Name& lhs, const Name& rhs) noexcept { return rhs < lhs; }
  friend bool operator<=(const Name& lhs, const Name& rhs) noexcept { return !(rhs < lhs); }
  friend bool operator>=(const Name& lhs, const Name& rhs) noexcept { return !(lhs < rhs); }

 private:
  explicit Name(std::string value) : value_(std::move(value)) {}
  std::string value_{};
};

// Number of failed unchecked() validations observed in this process (debug builds only).
[[nodiscard]] std::size_t unchecked_name_failure_count() noexcept;

}  // namespace congestion

#endif  // CONGESTION_CORE_NAME_HPP
