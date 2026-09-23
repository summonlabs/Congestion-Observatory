// Congestion Observatory - checked arithmetic for externally derived sizes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_CHECKED_HPP
#define CONGESTION_CORE_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "congestion/core/result.hpp"

namespace congestion {

// All sizes and counts that originate outside the process (wire frames, snapshot headers, JSON
// documents, CLI arguments) must be combined through these helpers. Overflow is a reported error,
// never undefined behaviour and never a silent wraparound that could defeat a bound check.

template <class T>
[[nodiscard]] constexpr bool checked_add(T lhs, T rhs, T& out) noexcept {
  static_assert(std::is_integral<T>::value, "checked_add requires an integral type");
  if constexpr (std::is_unsigned<T>::value) {
    if (rhs > static_cast<T>(std::numeric_limits<T>::max() - lhs)) {
      return false;
    }
  } else {
    if (rhs > 0 && lhs > static_cast<T>(std::numeric_limits<T>::max() - rhs)) {
      return false;
    }
    if (rhs < 0 && lhs < static_cast<T>(std::numeric_limits<T>::min() - rhs)) {
      return false;
    }
  }
  out = static_cast<T>(lhs + rhs);
  return true;
}

template <class T>
[[nodiscard]] constexpr bool checked_mul(T lhs, T rhs, T& out) noexcept {
  static_assert(std::is_integral<T>::value, "checked_mul requires an integral type");
  if (lhs == 0 || rhs == 0) {
    out = 0;
    return true;
  }
  if constexpr (std::is_unsigned<T>::value) {
    if (lhs > static_cast<T>(std::numeric_limits<T>::max() / rhs)) {
      return false;
    }
    out = static_cast<T>(lhs * rhs);
    return true;
  } else {
    const T max_value = std::numeric_limits<T>::max();
    const T min_value = std::numeric_limits<T>::min();
    if (lhs > 0) {
      if (rhs > 0) {
        if (lhs > max_value / rhs) return false;
      } else {
        if (rhs < min_value / lhs) return false;
      }
    } else {
      if (rhs > 0) {
        if (lhs < min_value / rhs) return false;
      } else {
        if (lhs != 0 && rhs < max_value / lhs) return false;
      }
    }
    out = static_cast<T>(lhs * rhs);
    return true;
  }
}

// Adds two unsigned values and reports whether the sum stayed within the supplied bound.
[[nodiscard]] inline Result<std::uint64_t> checked_add_within(std::uint64_t lhs, std::uint64_t rhs,
                                                             std::uint64_t bound,
                                                             const char* what) {
  std::uint64_t sum = 0;
  if (!checked_add(lhs, rhs, sum)) {
    return make_error(ErrorCode::kArithmeticOverflow, std::string("overflow adding ") + what);
  }
  if (sum > bound) {
    return make_error(ErrorCode::kLimitExceeded, std::string(what) + " exceeds configured bound",
                      "value=" + std::to_string(sum) + " bound=" + std::to_string(bound));
  }
  return sum;
}

// Saturating conversion helpers used when rendering counters for humans; never for decisions.
[[nodiscard]] inline std::uint32_t narrow_u32(std::uint64_t value) noexcept {
  return value > std::numeric_limits<std::uint32_t>::max()
             ? std::numeric_limits<std::uint32_t>::max()
             : static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline std::int64_t narrow_i64(std::uint64_t value) noexcept {
  return value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::max()
             : static_cast<std::int64_t>(value);
}

}  // namespace congestion

#endif  // CONGESTION_CORE_CHECKED_HPP
