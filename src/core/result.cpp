// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/result.hpp"

namespace congestion {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk: return "ok";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kOutOfRange: return "out_of_range";
    case ErrorCode::kLimitExceeded: return "limit_exceeded";
    case ErrorCode::kArithmeticOverflow: return "arithmetic_overflow";
    case ErrorCode::kNotFound: return "not_found";
    case ErrorCode::kAlreadyExists: return "already_exists";
    case ErrorCode::kConflict: return "conflict";
    case ErrorCode::kStaleEpoch: return "stale_epoch";
    case ErrorCode::kStaleGeneration: return "stale_generation";
    case ErrorCode::kStaleRevision: return "stale_revision";
    case ErrorCode::kReplayedSequence: return "replayed_sequence";
    case ErrorCode::kIncarnationChanged: return "incarnation_changed";
    case ErrorCode::kAuthorityTooLow: return "authority_too_low";
    case ErrorCode::kUnsupported: return "unsupported";
    case ErrorCode::kIncomplete: return "incomplete";
    case ErrorCode::kConflictingEvidence: return "conflicting_evidence";
    case ErrorCode::kIntegrityFailure: return "integrity_failure";
    case ErrorCode::kVersionMismatch: return "version_mismatch";
    case ErrorCode::kIoError: return "io_error";
    case ErrorCode::kCancelled: return "cancelled";
    case ErrorCode::kShuttingDown: return "shutting_down";
    case ErrorCode::kQueueFull: return "queue_full";
    case ErrorCode::kProtocolError: return "protocol_error";
    case ErrorCode::kUnauthorized: return "unauthorized";
    case ErrorCode::kPreconditionFailed: return "precondition_failed";
    case ErrorCode::kInternal: return "internal";
  }
  return "unknown";
}

std::string Error::describe() const {
  if (detail_.empty()) {
    return message_;
  }
  return message_ + " (" + detail_ + ")";
}

}  // namespace congestion
