// Congestion Observatory - explicit time and clock domains.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_TIME_HPP
#define CONGESTION_CORE_TIME_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "congestion/core/result.hpp"

namespace congestion {

class Duration;

// A Timestamp is a signed count of nanoseconds since the Unix epoch (1970-01-01T00:00:00Z).
// Negative values are legal and denote times before the epoch. Timestamps are always carried
// together with a ClockDomain: comparing or subtracting timestamps from different clock domains
// is a modelling error, and the runtime reports it as Unsupported rather than guessing.
class Timestamp {
 public:
  constexpr Timestamp() = default;
  constexpr explicit Timestamp(std::int64_t unix_nanos) : nanos_(unix_nanos) {}

  [[nodiscard]] static constexpr Timestamp from_unix_nanos(std::int64_t nanos) {
    return Timestamp(nanos);
  }
  [[nodiscard]] static Result<Timestamp> from_unix_seconds(std::int64_t seconds);
  [[nodiscard]] static Result<Timestamp> from_unix_millis(std::int64_t millis);
  [[nodiscard]] static Result<Timestamp> from_iso8601(std::string_view text);

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  // Age of this timestamp relative to the reference. Negative means "in the future".
  [[nodiscard]] Duration since(Timestamp reference) const noexcept;

  [[nodiscard]] std::string to_iso8601() const;

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.nanos_ == rhs.nanos_;
  }
  friend constexpr bool operator!=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.nanos_ < rhs.nanos_;
  }
  friend constexpr bool operator>(Timestamp lhs, Timestamp rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(Timestamp lhs, Timestamp rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs < rhs); }

 private:
  std::int64_t nanos_{0};
};

// Signed duration in nanoseconds. Duration arithmetic saturates only through checked helpers;
// plain operators are exact for the ranges the runtime admits (documented in limits.hpp).
class Duration {
 public:
  constexpr Duration() = default;
  constexpr explicit Duration(std::int64_t nanos) : nanos_(nanos) {}

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) { return Duration(nanos); }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t micros) {
    return Duration(micros * 1000);
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) {
    return Duration(millis * 1000000);
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) {
    return Duration(seconds * 1000000000);
  }
  [[nodiscard]] static constexpr Duration from_minutes(std::int64_t minutes) {
    return Duration(minutes * 60 * 1000000000);
  }
  [[nodiscard]] static constexpr Duration zero() { return Duration(0); }
  [[nodiscard]] static constexpr Duration max() { return Duration(INT64_MAX); }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }
  [[nodiscard]] double seconds_f() const noexcept {
    return static_cast<double>(nanos_) / 1e9;
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(Duration lhs, Duration rhs) noexcept {
    return lhs.nanos_ == rhs.nanos_;
  }
  friend constexpr bool operator!=(Duration lhs, Duration rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Duration lhs, Duration rhs) noexcept {
    return lhs.nanos_ < rhs.nanos_;
  }
  friend constexpr bool operator>(Duration lhs, Duration rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(Duration lhs, Duration rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(Duration lhs, Duration rhs) noexcept { return !(lhs < rhs); }

 private:
  std::int64_t nanos_{0};
};

[[nodiscard]] constexpr Timestamp operator+(Timestamp base, Duration delta) noexcept {
  return Timestamp(base.unix_nanos() + delta.nanos());
}
[[nodiscard]] constexpr Timestamp operator-(Timestamp base, Duration delta) noexcept {
  return Timestamp(base.unix_nanos() - delta.nanos());
}
[[nodiscard]] constexpr Duration operator-(Timestamp lhs, Timestamp rhs) noexcept {
  return Duration(lhs.unix_nanos() - rhs.unix_nanos());
}

// Where a timestamp came from. Freshness decisions require that the observed and received
// timestamps are commensurable; mixing domains yields Unknown freshness, never Fresh.
enum class ClockDomain : std::uint8_t {
  kUnknown = 0,
  kSourceMonotonic = 1,
  kSourceWallClock = 2,
  kCollectorMonotonic = 3,
  kCollectorWallClock = 4,
};

[[nodiscard]] std::string_view to_string(ClockDomain domain) noexcept;
[[nodiscard]] Result<ClockDomain> clock_domain_from_string(std::string_view text);

// Reads the collector wall clock. Used only where a physical clock is genuinely required
// (snapshot creation time, CLI output). Domain decisions never depend on it.
[[nodiscard]] Timestamp now_wall_clock() noexcept;

// Monotonic reference clock of the process, in nanoseconds since an unspecified origin.
[[nodiscard]] std::int64_t monotonic_nanos() noexcept;

}  // namespace congestion

#endif  // CONGESTION_CORE_TIME_HPP
