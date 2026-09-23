// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/name.hpp"

#include <atomic>

namespace congestion {
namespace {

std::atomic<std::size_t> g_unchecked_failures{0};

constexpr bool is_allowed_char(char c) noexcept {
  const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  if (alnum) {
    return true;
  }
  switch (c) {
    case '-':
    case '_':
    case '.':
    case ':':
    case '/':
    case '@':
    case '#':
    case '|':
    case '+':
    case '=':
    case ',':
      return true;
    default:
      return false;
  }
}

bool is_separator(char c) noexcept {
  return c == '/' || c == ':' || c == '|' || c == ',' || c == '.';
}

// Validates and returns the canonical text. The caller (a Name member) turns it into a Name.
Result<std::string> validate(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "name must not be empty");
  }
  if (text.size() > Name::kMaxLength) {
    return make_error(ErrorCode::kLimitExceeded, "name exceeds maximum length",
                      "length=" + std::to_string(text.size()) +
                          " max=" + std::to_string(Name::kMaxLength));
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (!is_allowed_char(text[i])) {
      return make_error(ErrorCode::kInvalidArgument, "name contains a non canonical character",
                        "offset=" + std::to_string(i));
    }
  }
  if (is_separator(text.front())) {
    return make_error(ErrorCode::kInvalidArgument, "name must not start with a separator");
  }
  if (is_separator(text.back())) {
    return make_error(ErrorCode::kInvalidArgument, "name must not end with a separator");
  }
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (is_separator(text[i]) && is_separator(text[i - 1])) {
      return make_error(ErrorCode::kInvalidArgument, "name contains an empty segment",
                        "offset=" + std::to_string(i));
    }
  }
  return std::string(text);
}

}  // namespace

Result<Name> Name::parse(std::string_view text) {
  auto validated = validate(text);
  if (!validated.ok()) {
    return validated.error();
  }
  return Name(std::move(validated.value()));
}

Name Name::unchecked(std::string_view text) noexcept {
  auto validated = validate(text);
  if (!validated.ok()) {
    g_unchecked_failures.fetch_add(1, std::memory_order_relaxed);
    return Name();
  }
  return Name(std::move(validated.value()));
}

std::size_t unchecked_name_failure_count() noexcept {
  return g_unchecked_failures.load(std::memory_order_relaxed);
}

}  // namespace congestion
