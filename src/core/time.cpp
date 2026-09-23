// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/time.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "congestion/core/checked.hpp"

namespace congestion {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000;
constexpr std::int64_t kNanosPerDay = 86400 * kNanosPerSecond;

// Howard Hinnant's civil calendar algorithms: exact for the whole representable range and free
// of locale or time zone dependence.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = y + (month <= 2 ? 1 : 0);
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

Result<int> read_int(std::string_view text, std::size_t offset, std::size_t length) {
  if (offset + length > text.size()) {
    return make_error(ErrorCode::kInvalidArgument, "timestamp field truncated");
  }
  int value = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = text[offset + i];
    if (!is_digit(c)) {
      return make_error(ErrorCode::kInvalidArgument, "timestamp contains a non digit");
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

bool is_leap(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) noexcept {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap(year)) {
    return 29;
  }
  return kDays[month - 1];
}

}  // namespace

Result<Timestamp> Timestamp::from_unix_seconds(std::int64_t seconds) {
  std::int64_t nanos = 0;
  if (!checked_mul<std::int64_t>(seconds, kNanosPerSecond, nanos)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp seconds overflow");
  }
  return Timestamp(nanos);
}

Result<Timestamp> Timestamp::from_unix_millis(std::int64_t millis) {
  std::int64_t nanos = 0;
  if (!checked_mul<std::int64_t>(millis, 1000000, nanos)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp millis overflow");
  }
  return Timestamp(nanos);
}

Timestamp now_wall_clock() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::int64_t monotonic_nanos() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

Duration Timestamp::since(Timestamp reference) const noexcept {
  return Duration(nanos_ - reference.nanos_);
}

std::string Timestamp::to_iso8601() const {
  std::int64_t seconds = nanos_ / kNanosPerSecond;
  std::int64_t remainder = nanos_ % kNanosPerSecond;
  if (remainder < 0) {
    remainder += kNanosPerSecond;
    seconds -= 1;
  }
  const std::int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
  std::int64_t second_of_day = seconds - days * 86400;
  std::int64_t year = 0;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);
  const int hour = static_cast<int>(second_of_day / 3600);
  const int minute = static_cast<int>((second_of_day % 3600) / 60);
  const int second = static_cast<int>(second_of_day % 60);

  char buffer[64];
  if (remainder == 0) {
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02d:%02d:%02dZ",
                  static_cast<long long>(year), month, day, hour, minute, second);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02d:%02d:%02d.%09lldZ",
                  static_cast<long long>(year), month, day, hour, minute, second,
                  static_cast<long long>(remainder));
  }
  return std::string(buffer);
}

Result<Timestamp> Timestamp::from_iso8601(std::string_view text) {
  // Accepted form: YYYY-MM-DDTHH:MM:SS[.fraction]Z  (UTC only, no offsets).
  if (text.size() < 20) {
    return make_error(ErrorCode::kInvalidArgument, "timestamp is too short");
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return make_error(ErrorCode::kInvalidArgument, "timestamp layout is not ISO-8601 UTC");
  }
  if (text.back() != 'Z') {
    return make_error(ErrorCode::kUnsupported,
                      "only UTC timestamps are accepted; offsets need an explicit clock domain");
  }
  auto year = read_int(text, 0, 4);
  auto month = read_int(text, 5, 2);
  auto day = read_int(text, 8, 2);
  auto hour = read_int(text, 11, 2);
  auto minute = read_int(text, 14, 2);
  auto second = read_int(text, 17, 2);
  if (!year.ok()) return year.error();
  if (!month.ok()) return month.error();
  if (!day.ok()) return day.error();
  if (!hour.ok()) return hour.error();
  if (!minute.ok()) return minute.error();
  if (!second.ok()) return second.error();

  if (month.value() < 1 || month.value() > 12) {
    return make_error(ErrorCode::kOutOfRange, "timestamp month out of range");
  }
  if (day.value() < 1 || day.value() > days_in_month(year.value(), month.value())) {
    return make_error(ErrorCode::kOutOfRange, "timestamp day out of range");
  }
  if (hour.value() > 23 || minute.value() > 59 || second.value() > 59) {
    return make_error(ErrorCode::kOutOfRange, "timestamp time of day out of range");
  }

  std::int64_t fraction = 0;
  std::size_t index = 19;
  if (text.size() > 20) {
    if (text[19] != '.') {
      return make_error(ErrorCode::kInvalidArgument, "timestamp fraction separator missing");
    }
    std::size_t digits = 0;
    for (index = 20; index + 1 < text.size(); ++index) {
      const char c = text[index];
      if (!is_digit(c)) {
        return make_error(ErrorCode::kInvalidArgument, "timestamp fraction contains a non digit");
      }
      if (digits < 9) {
        fraction = fraction * 10 + (c - '0');
        ++digits;
      }
    }
    while (digits < 9) {
      fraction *= 10;
      ++digits;
    }
  }

  const std::int64_t days = days_from_civil(year.value(), static_cast<unsigned>(month.value()),
                                            static_cast<unsigned>(day.value()));
  std::int64_t seconds = 0;
  if (!checked_mul<std::int64_t>(days, 86400, seconds)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp day count overflow");
  }
  std::int64_t day_seconds = 0;
  if (!checked_mul<std::int64_t>(static_cast<std::int64_t>(hour.value()), 3600, day_seconds)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp hour overflow");
  }
  std::int64_t add = 0;
  if (!checked_add<std::int64_t>(day_seconds,
                                 static_cast<std::int64_t>(minute.value()) * 60 + second.value(),
                                 add)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp minute overflow");
  }
  std::int64_t total_seconds = 0;
  if (!checked_add<std::int64_t>(seconds, add, total_seconds)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp seconds overflow");
  }
  std::int64_t nanos = 0;
  if (!checked_mul<std::int64_t>(total_seconds, kNanosPerSecond, nanos)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp nanoseconds overflow");
  }
  std::int64_t total = 0;
  if (!checked_add<std::int64_t>(nanos, fraction, total)) {
    return make_error(ErrorCode::kArithmeticOverflow, "timestamp fraction overflow");
  }
  return Timestamp(total);
}

std::string Duration::to_string() const {
  const bool negative = nanos_ < 0;
  std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(nanos_ + 1)) + 1u : static_cast<std::uint64_t>(nanos_);
  const std::uint64_t seconds = magnitude / 1000000000ULL;
  const std::uint64_t fraction = magnitude % 1000000000ULL;
  std::string out = negative ? "-" : "";
  out += std::to_string(seconds);
  if (fraction != 0) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), ".%09llu", static_cast<unsigned long long>(fraction));
    std::string frac(buffer);
    while (!frac.empty() && frac.back() == '0') {
      frac.pop_back();
    }
    out += frac;
  }
  out += "s";
  return out;
}

std::string_view to_string(ClockDomain domain) noexcept {
  switch (domain) {
    case ClockDomain::kUnknown: return "unknown";
    case ClockDomain::kSourceMonotonic: return "source_monotonic";
    case ClockDomain::kSourceWallClock: return "source_wall_clock";
    case ClockDomain::kCollectorMonotonic: return "collector_monotonic";
    case ClockDomain::kCollectorWallClock: return "collector_wall_clock";
  }
  return "unknown";
}

Result<ClockDomain> clock_domain_from_string(std::string_view text) {
  if (text == "unknown") return ClockDomain::kUnknown;
  if (text == "source_monotonic") return ClockDomain::kSourceMonotonic;
  if (text == "source_wall_clock") return ClockDomain::kSourceWallClock;
  if (text == "collector_monotonic") return ClockDomain::kCollectorMonotonic;
  if (text == "collector_wall_clock") return ClockDomain::kCollectorWallClock;
  return make_error(ErrorCode::kInvalidArgument, "unknown clock domain", std::string(text));
}

}  // namespace congestion
