// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/model/generation.hpp"

namespace congestion {

std::string GenerationVector::str() const {
  return std::to_string(epoch.value()) + "/" + std::to_string(generation.value()) + "/" +
         std::to_string(revision.value());
}

GenerationOrder compare_generation(const GenerationVector& candidate,
                                   const GenerationVector& reference) noexcept {
  if (candidate.epoch == reference.epoch) {
    if (candidate.generation == reference.generation) {
      if (candidate.revision == reference.revision) {
        return GenerationOrder::kEqual;
      }
      return candidate.revision < reference.revision ? GenerationOrder::kOlder
                                                     : GenerationOrder::kNewer;
    }
    return candidate.generation < reference.generation ? GenerationOrder::kOlder
                                                       : GenerationOrder::kNewer;
  }
  if (candidate.epoch < reference.epoch) {
    return GenerationOrder::kOlder;
  }
  // A higher epoch must not lower the generation: the re-baseline has to be explicit, otherwise
  // the vector is inconsistent and cannot be ordered.
  if (candidate.generation < reference.generation) {
    return GenerationOrder::kIncomparable;
  }
  return GenerationOrder::kNewer;
}

std::string_view to_string(AuthorityLevel level) noexcept {
  switch (level) {
    case AuthorityLevel::kUnknown: return "unknown";
    case AuthorityLevel::kUnverified: return "unverified";
    case AuthorityLevel::kReported: return "reported";
    case AuthorityLevel::kMeasured: return "measured";
    case AuthorityLevel::kCorroborated: return "corroborated";
  }
  return "unknown";
}

Result<AuthorityLevel> authority_from_string(std::string_view text) {
  if (text == "unknown") return AuthorityLevel::kUnknown;
  if (text == "unverified") return AuthorityLevel::kUnverified;
  if (text == "reported") return AuthorityLevel::kReported;
  if (text == "measured") return AuthorityLevel::kMeasured;
  if (text == "corroborated") return AuthorityLevel::kCorroborated;
  return make_error(ErrorCode::kInvalidArgument, "unknown authority level", std::string(text));
}

std::string_view to_string(FenceDecision decision) noexcept {
  switch (decision) {
    case FenceDecision::kAcceptedFirstObservation: return "accepted_first_observation";
    case FenceDecision::kAcceptedInOrder: return "accepted_in_order";
    case FenceDecision::kAcceptedGapDetected: return "accepted_gap_detected";
    case FenceDecision::kAcceptedReboot: return "accepted_reboot";
    case FenceDecision::kAcceptedEpochAdvance: return "accepted_epoch_advance";
    case FenceDecision::kAcceptedGenerationAdvance: return "accepted_generation_advance";
    case FenceDecision::kRejectedUnknownSource: return "rejected_unknown_source";
    case FenceDecision::kRejectedStaleEpoch: return "rejected_stale_epoch";
    case FenceDecision::kRejectedStaleGeneration: return "rejected_stale_generation";
    case FenceDecision::kRejectedStaleRevision: return "rejected_stale_revision";
    case FenceDecision::kRejectedIncomparableGeneration: return "rejected_incomparable_generation";
    case FenceDecision::kRejectedReplayedSequence: return "rejected_replayed_sequence";
    case FenceDecision::kRejectedStaleIncarnation: return "rejected_stale_incarnation";
    case FenceDecision::kRejectedAuthorityTooLow: return "rejected_authority_too_low";
    case FenceDecision::kRejectedInconsistentBoot: return "rejected_inconsistent_boot";
  }
  return "rejected_unknown_source";
}

bool fence_accepted(FenceDecision decision) noexcept {
  switch (decision) {
    case FenceDecision::kAcceptedFirstObservation:
    case FenceDecision::kAcceptedInOrder:
    case FenceDecision::kAcceptedGapDetected:
    case FenceDecision::kAcceptedReboot:
    case FenceDecision::kAcceptedEpochAdvance:
    case FenceDecision::kAcceptedGenerationAdvance:
      return true;
    default:
      return false;
  }
}

FenceOutcome evaluate_fence(const FenceVector& incoming, AuthorityLevel authority,
                            AuthorityLevel minimum_authority,
                            const SourceFenceState& previous) noexcept {
  FenceOutcome outcome;
  if (!incoming.source.valid()) {
    outcome.decision = FenceDecision::kRejectedUnknownSource;
    outcome.detail = "source identity missing";
    return outcome;
  }
  if (static_cast<std::uint8_t>(authority) < static_cast<std::uint8_t>(minimum_authority)) {
    outcome.decision = FenceDecision::kRejectedAuthorityTooLow;
    outcome.detail = std::string("authority=") + std::string(to_string(authority)) +
                     " required=" + std::string(to_string(minimum_authority));
    return outcome;
  }
  if (!previous.has_state) {
    outcome.decision = FenceDecision::kAcceptedFirstObservation;
    outcome.detail = "first observation from this source";
    return outcome;
  }

  const FenceVector& last = previous.last;
  if (incoming.boot != last.boot) {
    if (incoming.boot < last.boot) {
      outcome.decision = FenceDecision::kRejectedStaleIncarnation;
      outcome.detail = "boot=" + incoming.boot.str() + " superseded by boot=" + last.boot.str();
      return outcome;
    }
    outcome.decision = FenceDecision::kAcceptedReboot;
    outcome.detail = "boot=" + incoming.boot.str() + " supersedes boot=" + last.boot.str();
    outcome.liveness_reset = true;
    return outcome;
  }
  if (incoming.incarnation < last.incarnation) {
    outcome.decision = FenceDecision::kRejectedStaleIncarnation;
    outcome.detail = "incarnation=" + incoming.incarnation.str() + " superseded by " +
                     last.incarnation.str();
    return outcome;
  }
  if (incoming.incarnation > last.incarnation) {
    outcome.decision = FenceDecision::kRejectedInconsistentBoot;
    outcome.detail = "incarnation advanced without a new boot id";
    return outcome;
  }

  switch (compare_generation(incoming.gen, last.gen)) {
    case GenerationOrder::kOlder:
      // The most specific difference is reported: epoch, then generation, then revision.
      if (incoming.gen.epoch < last.gen.epoch) {
        outcome.decision = FenceDecision::kRejectedStaleEpoch;
        outcome.detail = "epoch=" + incoming.gen.epoch.str() + " superseded by " +
                         last.gen.epoch.str();
      } else if (incoming.gen.generation < last.gen.generation) {
        outcome.decision = FenceDecision::kRejectedStaleGeneration;
        outcome.detail = "generation=" + incoming.gen.generation.str() + " superseded by " +
                         last.gen.generation.str();
      } else {
        outcome.decision = FenceDecision::kRejectedStaleRevision;
        outcome.detail = "revision=" + incoming.gen.revision.str() + " superseded by " +
                         last.gen.revision.str();
      }
      return outcome;
    case GenerationOrder::kIncomparable:
      outcome.decision = FenceDecision::kRejectedIncomparableGeneration;
      outcome.detail = "candidate=" + incoming.gen.str() + " reference=" + last.gen.str();
      return outcome;
    case GenerationOrder::kNewer:
      if (incoming.gen.epoch > last.gen.epoch) {
        outcome.decision = FenceDecision::kAcceptedEpochAdvance;
        outcome.detail = "epoch=" + incoming.gen.epoch.str() + " supersedes " + last.gen.epoch.str();
        return outcome;
      }
      if (incoming.gen.generation > last.gen.generation) {
        outcome.decision = FenceDecision::kAcceptedGenerationAdvance;
        outcome.detail = "generation=" + incoming.gen.generation.str() + " supersedes " +
                         last.gen.generation.str();
        return outcome;
      }
      // A revision advance stays inside the same generation: sequencing still has to be monotone.
      break;
    case GenerationOrder::kEqual:
      break;
  }

  // Same epoch/generation/revision: the sequence must advance monotonically.
  if (incoming.sequence <= last.sequence) {
    outcome.decision = FenceDecision::kRejectedReplayedSequence;
    outcome.detail = "sequence=" + incoming.sequence.str() + " not after " + last.sequence.str();
    return outcome;
  }
  const std::uint64_t expected = last.sequence.value() + 1;
  if (incoming.sequence.value() > expected) {
    outcome.decision = FenceDecision::kAcceptedGapDetected;
    outcome.sequence_gap = true;
    outcome.gap_size = incoming.sequence.value() - expected;
    outcome.detail = "missing " + std::to_string(outcome.gap_size) + " sequence numbers";
    return outcome;
  }
  outcome.decision = FenceDecision::kAcceptedInOrder;
  outcome.detail = "sequence=" + incoming.sequence.str();
  return outcome;
}

}  // namespace congestion
