// Congestion Observatory - deterministic JSON writing and bounded strict parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_JSON_HPP
#define CONGESTION_CORE_JSON_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"

namespace congestion {

class JsonValue;

// Object members preserve insertion order so that serialisation is byte-reproducible: callers
// build members in a fixed order and the writer never reorders them.
class JsonObject {
 public:
  JsonObject() = default;

  // Inserts a member, or replaces the value of an existing key. Logarithmic in the member count:
  // a document with many keys must not become quadratic to parse.
  JsonObject& set(std::string key, JsonValue value);
  // Inserts only when the key is new. Returns false (and changes nothing) on a duplicate, which is
  // what strict document parsing needs.
  bool try_insert(std::string key, JsonValue value);
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
  [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
  [[nodiscard]] bool empty() const noexcept { return members_.empty(); }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
    return members_;
  }

 private:
  void reindex();

  // Insertion order is preserved for deterministic serialisation; the index exists so that
  // duplicate detection is not a linear scan.
  std::vector<std::pair<std::string, JsonValue>> members_{};
  std::map<std::string, std::size_t, std::less<>> index_{};
};

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;

  JsonValue() = default;  // null
  JsonValue(std::nullptr_t) {}  // NOLINT
  JsonValue(bool value) : storage_(value) {}  // NOLINT
  JsonValue(std::int64_t value) : storage_(value) {}  // NOLINT
  JsonValue(double value) : storage_(value) {}  // NOLINT
  JsonValue(std::string value) : storage_(std::move(value)) {}  // NOLINT
  JsonValue(const char* value) : storage_(std::string(value)) {}  // NOLINT
  JsonValue(Array value) : storage_(std::move(value)) {}  // NOLINT
  JsonValue(JsonObject value) : storage_(std::move(value)) {}  // NOLINT

  [[nodiscard]] bool is_null() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] bool is_bool() const noexcept { return storage_.index() == 1; }
  [[nodiscard]] bool is_int() const noexcept { return storage_.index() == 2; }
  [[nodiscard]] bool is_double() const noexcept { return storage_.index() == 3; }
  [[nodiscard]] bool is_string() const noexcept { return storage_.index() == 4; }
  [[nodiscard]] bool is_array() const noexcept { return storage_.index() == 5; }
  [[nodiscard]] bool is_object() const noexcept { return storage_.index() == 6; }

  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
  [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] double as_double(double fallback = 0.0) const noexcept;
  [[nodiscard]] const std::string& as_string() const noexcept;
  [[nodiscard]] const Array& as_array() const noexcept;
  [[nodiscard]] const JsonObject& as_object() const noexcept;

  [[nodiscard]] const JsonValue* member(std::string_view key) const noexcept;

  // Serialises with stable member order and shortest round-trip number formatting.
  [[nodiscard]] std::string dump(int indent = 0) const;

 private:
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, JsonObject> storage_{
      nullptr};
};

// Strict parser. The input must be a single well formed JSON document; trailing content is an
// error. Depth, node count and input size are bounded by Limits. Numbers are never silently
// truncated: an out-of-range integer literal is reported.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, const Limits& limits);

// Escapes a string for inclusion in JSON output.
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace congestion

#endif  // CONGESTION_CORE_JSON_HPP
