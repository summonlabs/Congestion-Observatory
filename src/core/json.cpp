// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace congestion {
namespace {

void append_indent(std::string& out, int indent, int level) {
  if (indent <= 0) {
    return;
  }
  out.push_back('\n');
  out.append(static_cast<std::size_t>(indent * level), ' ');
}

void write_double(std::string& out, double value) {
  if (!std::isfinite(value)) {
    // JSON has no representation for NaN or infinity; the writer refuses to invent one.
    out += "null";
    return;
  }
  char buffer[40];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) {
    out += "null";
    return;
  }
  out.append(buffer, result.ptr);
}

void write_number(std::string& out, std::int64_t value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, result.ptr);
}

void write_value(const JsonValue& value, std::string& out, int indent, int level) {
  if (value.is_null()) {
    out += "null";
  } else if (value.is_bool()) {
    out += value.as_bool() ? "true" : "false";
  } else if (value.is_int()) {
    write_number(out, value.as_int());
  } else if (value.is_double()) {
    write_double(out, value.as_double());
  } else if (value.is_string()) {
    out.push_back('"');
    out += json_escape(value.as_string());
    out.push_back('"');
  } else if (value.is_array()) {
    const auto& items = value.as_array();
    if (items.empty()) {
      out += "[]";
      return;
    }
    out.push_back('[');
    bool first = true;
    for (const auto& item : items) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      append_indent(out, indent, level + 1);
      write_value(item, out, indent, level + 1);
    }
    append_indent(out, indent, level);
    out.push_back(']');
  } else {
    const auto& members = value.as_object().members();
    if (members.empty()) {
      out += "{}";
      return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& member : members) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      append_indent(out, indent, level + 1);
      out.push_back('"');
      out += json_escape(member.first);
      out.push_back('"');
      out.push_back(':');
      if (indent > 0) {
        out.push_back(' ');
      }
      write_value(member.second, out, indent, level + 1);
    }
    append_indent(out, indent, level);
    out.push_back('}');
  }
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class Parser {
 public:
  Parser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

  [[nodiscard]] Result<JsonValue> parse_document() {
    if (text_.size() > limits_.max_document_bytes) {
      return make_error(ErrorCode::kLimitExceeded, "document exceeds the configured size bound",
                        "bytes=" + std::to_string(text_.size()));
    }
    skip_whitespace();
    auto value = parse_value(0);
    if (!value.ok()) {
      return value.error();
    }
    skip_whitespace();
    if (position_ != text_.size()) {
      return make_error(ErrorCode::kInvalidArgument, "trailing content after the JSON document",
                        "offset=" + std::to_string(position_));
    }
    return value;
  }

 private:
  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] Result<char> peek() {
    if (position_ >= text_.size()) {
      return make_error(ErrorCode::kInvalidArgument, "unexpected end of JSON input");
    }
    return text_[position_];
  }

  [[nodiscard]] Status expect(char expected) {
    if (position_ >= text_.size() || text_[position_] != expected) {
      return Status(make_error(ErrorCode::kInvalidArgument, "unexpected JSON token",
                               std::string("expected=") + expected +
                                   " offset=" + std::to_string(position_)));
    }
    ++position_;
    return Status{};
  }

  [[nodiscard]] Status count_node() {
    ++nodes_;
    if (nodes_ > limits_.max_json_nodes) {
      return Status(make_error(ErrorCode::kLimitExceeded, "JSON node budget exhausted"));
    }
    return Status{};
  }

  [[nodiscard]] Result<JsonValue> parse_value(int depth) {
    if (depth > static_cast<int>(limits_.max_json_depth)) {
      return make_error(ErrorCode::kLimitExceeded, "JSON nesting depth exceeded");
    }
    const Status counted = count_node();
    if (!counted.ok()) {
      return counted.error();
    }
    auto next = peek();
    if (!next.ok()) {
      return next.error();
    }
    switch (next.value()) {
      case '{': return parse_object(depth);
      case '[': return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text.ok()) {
          return text.error();
        }
        return JsonValue(std::move(text.value()));
      }
      case 't': return parse_literal("true", JsonValue(true));
      case 'f': return parse_literal("false", JsonValue(false));
      case 'n': return parse_literal("null", JsonValue(nullptr));
      default: return parse_number();
    }
  }

  [[nodiscard]] Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
    if (text_.size() - position_ < literal.size() ||
        text_.substr(position_, literal.size()) != literal) {
      return make_error(ErrorCode::kInvalidArgument, "invalid JSON literal",
                        "offset=" + std::to_string(position_));
    }
    position_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') {
      ++position_;
    }
    bool is_integer = true;
    bool any_digit = false;
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c >= '0' && c <= '9') {
        any_digit = true;
        ++position_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        is_integer = false;
        ++position_;
      } else {
        break;
      }
    }
    if (!any_digit) {
      return make_error(ErrorCode::kInvalidArgument, "invalid JSON number",
                        "offset=" + std::to_string(start));
    }
    const std::string_view token = text_.substr(start, position_ - start);
    if (is_integer) {
      std::int64_t value = 0;
      const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
      if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
        return JsonValue(value);
      }
      if (result.ec == std::errc::result_out_of_range) {
        return make_error(ErrorCode::kOutOfRange, "JSON integer literal out of range",
                          std::string(token));
      }
      return make_error(ErrorCode::kInvalidArgument, "invalid JSON integer", std::string(token));
    }
    double value = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
      return make_error(ErrorCode::kInvalidArgument, "invalid JSON number", std::string(token));
    }
    if (!std::isfinite(value)) {
      return make_error(ErrorCode::kOutOfRange, "JSON number is not finite", std::string(token));
    }
    return JsonValue(value);
  }

  [[nodiscard]] Result<std::uint32_t> parse_hex4() {
    if (position_ + 4 > text_.size()) {
      return make_error(ErrorCode::kInvalidArgument, "truncated unicode escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return make_error(ErrorCode::kInvalidArgument, "invalid hexadecimal digit in escape");
      }
    }
    return value;
  }

  [[nodiscard]] Result<std::string> parse_string() {
    const Status opened = expect('"');
    if (!opened.ok()) {
      return opened.error();
    }
    std::string out;
    while (true) {
      if (position_ >= text_.size()) {
        return make_error(ErrorCode::kInvalidArgument, "unterminated JSON string");
      }
      const char c = text_[position_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        if (static_cast<unsigned char>(c) < 0x20) {
          return make_error(ErrorCode::kInvalidArgument, "control character in JSON string");
        }
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) {
        return make_error(ErrorCode::kInvalidArgument, "unterminated JSON escape");
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          auto code_point = parse_hex4();
          if (!code_point.ok()) {
            return code_point.error();
          }
          std::uint32_t value = code_point.value();
          if (value >= 0xD800 && value <= 0xDBFF) {
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return make_error(ErrorCode::kInvalidArgument,
                                "high surrogate without a low surrogate");
            }
            position_ += 2;
            auto low = parse_hex4();
            if (!low.ok()) {
              return low.error();
            }
            if (low.value() < 0xDC00 || low.value() > 0xDFFF) {
              return make_error(ErrorCode::kInvalidArgument, "invalid low surrogate");
            }
            value = 0x10000 + ((value - 0xD800) << 10) + (low.value() - 0xDC00);
          } else if (value >= 0xDC00 && value <= 0xDFFF) {
            return make_error(ErrorCode::kInvalidArgument, "unexpected low surrogate");
          }
          append_utf8(out, value);
          break;
        }
        default:
          return make_error(ErrorCode::kInvalidArgument, "unknown JSON escape sequence");
      }
    }
  }

  [[nodiscard]] Result<JsonValue> parse_array(int depth) {
    const Status opened = expect('[');
    if (!opened.ok()) {
      return opened.error();
    }
    JsonValue::Array items;
    skip_whitespace();
    auto next = peek();
    if (!next.ok()) {
      return next.error();
    }
    if (next.value() == ']') {
      ++position_;
      return JsonValue(std::move(items));
    }
    while (true) {
      skip_whitespace();
      auto item = parse_value(depth + 1);
      if (!item.ok()) {
        return item.error();
      }
      items.push_back(std::move(item.value()));
      skip_whitespace();
      auto token = peek();
      if (!token.ok()) {
        return token.error();
      }
      if (token.value() == ',') {
        ++position_;
        continue;
      }
      if (token.value() == ']') {
        ++position_;
        return JsonValue(std::move(items));
      }
      return make_error(ErrorCode::kInvalidArgument, "expected a comma or array terminator");
    }
  }

  [[nodiscard]] Result<JsonValue> parse_object(int depth) {
    const Status opened = expect('{');
    if (!opened.ok()) {
      return opened.error();
    }
    JsonObject object;
    skip_whitespace();
    auto next = peek();
    if (!next.ok()) {
      return next.error();
    }
    if (next.value() == '}') {
      ++position_;
      return JsonValue(std::move(object));
    }
    while (true) {
      skip_whitespace();
      auto key = parse_string();
      if (!key.ok()) {
        return key.error();
      }
      skip_whitespace();
      const Status colon = expect(':');
      if (!colon.ok()) {
        return colon.error();
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value.ok()) {
        return value.error();
      }
      if (!object.try_insert(std::move(key.value()), std::move(value.value()))) {
        return make_error(ErrorCode::kInvalidArgument, "duplicate JSON object key");
      }
      skip_whitespace();
      auto token = peek();
      if (!token.ok()) {
        return token.error();
      }
      if (token.value() == ',') {
        ++position_;
        continue;
      }
      if (token.value() == '}') {
        ++position_;
        return JsonValue(std::move(object));
      }
      return make_error(ErrorCode::kInvalidArgument, "expected a comma or object terminator");
    }
  }

  std::string_view text_;
  Limits limits_;
  std::size_t position_{0};
  std::size_t nodes_{0};
};

}  // namespace

void JsonObject::reindex() {
  index_.clear();
  for (std::size_t i = 0; i < members_.size(); ++i) {
    index_.emplace(members_[i].first, i);
  }
}

bool JsonObject::try_insert(std::string key, JsonValue value) {
  if (index_.size() != members_.size()) {
    reindex();
  }
  if (index_.find(key) != index_.end()) {
    return false;
  }
  members_.emplace_back(key, std::move(value));
  index_.emplace(std::move(key), members_.size() - 1);
  return true;
}

JsonObject& JsonObject::set(std::string key, JsonValue value) {
  if (index_.size() != members_.size()) {
    reindex();
  }
  const auto it = index_.find(key);
  if (it != index_.end() && it->second < members_.size()) {
    members_[it->second].second = std::move(value);
    return *this;
  }
  members_.emplace_back(key, std::move(value));
  index_.emplace(std::move(key), members_.size() - 1);
  return *this;
}

const JsonValue* JsonObject::find(std::string_view key) const noexcept {
  if (index_.size() != members_.size()) {
    // Copies of an object always carry a consistent index; this guard only protects against a
    // partially constructed object observed through a const reference.
    return nullptr;
  }
  const auto it = index_.find(key);
  if (it == index_.end() || it->second >= members_.size()) {
    return nullptr;
  }
  return &members_[it->second].second;
}

bool JsonValue::as_bool(bool fallback) const noexcept {
  if (const auto* value = std::get_if<bool>(&storage_)) {
    return *value;
  }
  return fallback;
}

std::int64_t JsonValue::as_int(std::int64_t fallback) const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return *value;
  }
  if (const auto* value = std::get_if<double>(&storage_)) {
    return static_cast<std::int64_t>(*value);
  }
  return fallback;
}

double JsonValue::as_double(double fallback) const noexcept {
  if (const auto* value = std::get_if<double>(&storage_)) {
    return *value;
  }
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return static_cast<double>(*value);
  }
  return fallback;
}

const std::string& JsonValue::as_string() const noexcept {
  static const std::string kEmpty;
  if (const auto* value = std::get_if<std::string>(&storage_)) {
    return *value;
  }
  return kEmpty;
}

const JsonValue::Array& JsonValue::as_array() const noexcept {
  static const Array kEmpty;
  if (const auto* value = std::get_if<Array>(&storage_)) {
    return *value;
  }
  return kEmpty;
}

const JsonObject& JsonValue::as_object() const noexcept {
  static const JsonObject kEmpty;
  if (const auto* value = std::get_if<JsonObject>(&storage_)) {
    return *value;
  }
  return kEmpty;
}

const JsonValue* JsonValue::member(std::string_view key) const noexcept {
  if (const auto* object = std::get_if<JsonObject>(&storage_)) {
    return object->find(key);
  }
  return nullptr;
}

std::string JsonValue::dump(int indent) const {
  std::string out;
  write_value(*this, out, indent, 0);
  return out;
}

Result<JsonValue> parse_json(std::string_view text, const Limits& limits) {
  Parser parser(text, limits);
  return parser.parse_document();
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

}  // namespace congestion
