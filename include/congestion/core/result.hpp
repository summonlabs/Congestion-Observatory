// Congestion Observatory - explicit error and result plumbing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_RESULT_HPP
#define CONGESTION_CORE_RESULT_HPP

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace congestion {

// Every failure mode the runtime can report. Codes are stable identifiers used by tests,
// explanations and exports; they must not be renumbered casually.
enum class ErrorCode : std::uint16_t {
  kOk = 0,
  kInvalidArgument,
  kOutOfRange,
  kLimitExceeded,
  kArithmeticOverflow,
  kNotFound,
  kAlreadyExists,
  kConflict,
  kStaleEpoch,
  kStaleGeneration,
  kStaleRevision,
  kReplayedSequence,
  kIncarnationChanged,
  kAuthorityTooLow,
  kUnsupported,
  kIncomplete,
  kConflictingEvidence,
  kIntegrityFailure,
  kVersionMismatch,
  kIoError,
  kCancelled,
  kShuttingDown,
  kQueueFull,
  kProtocolError,
  kUnauthorized,
  kPreconditionFailed,
  kInternal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Human readable error carrier. Message is a static-ish description, detail carries context.
class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}
  Error(ErrorCode code, std::string message, std::string detail)
      : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }

  // "message (detail)" when detail is present, otherwise "message".
  [[nodiscard]] std::string describe() const;

 private:
  ErrorCode code_{ErrorCode::kOk};
  std::string message_{};
  std::string detail_{};
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}
[[nodiscard]] inline Error make_error(ErrorCode code, std::string message, std::string detail) {
  return Error(code, std::move(message), std::move(detail));
}

// Status: result of an operation that produces no value.
class Status {
 public:
  Status() = default;  // success
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }

 private:
  Error error_{};
};

[[nodiscard]] inline Status ok_status() { return Status{}; }

// Result<T>: either a value or an Error. value() must only be called when ok().
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Error& error() const noexcept { return std::get<1>(storage_); }
  [[nodiscard]] ErrorCode code() const noexcept { return ok() ? ErrorCode::kOk : error().code(); }

  // Value if present, otherwise the supplied fallback.
  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

}  // namespace congestion

#endif  // CONGESTION_CORE_RESULT_HPP
