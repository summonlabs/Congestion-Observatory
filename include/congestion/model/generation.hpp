// Congestion Observatory - generation, incarnation and fencing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_MODEL_GENERATION_HPP
#define CONGESTION_MODEL_GENERATION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "congestion/core/strong_id.hpp"
#include "congestion/core/time.hpp"
#include "congestion/model/identities.hpp"

namespace congestion {

// Epoch -> Generation -> Revision is the compatibility vector of the whole system. An epoch
// changes when the observation model itself changes (topology re-baseline, policy swap); a
// generation changes when the observed fabric is re-instantiated (device reboot, fabric merge);
// a revision changes on every incremental update within a generation.
struct GenerationVector {
  Epoch epoch{};
  Generation generation{};
  Revision revision{};

  friend bool operator==(const GenerationVector& lhs, const GenerationVector& rhs) noexcept {
    return lhs.epoch == rhs.epoch && lhs.generation == rhs.generation &&
           lhs.revision == rhs.revision;
  }
  friend bool operator!=(const GenerationVector& lhs, const GenerationVector& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const GenerationVector& lhs, const GenerationVector& rhs) noexcept {
    if (lhs.epoch != rhs.epoch) return lhs.epoch < rhs.epoch;
    if (lhs.generation != rhs.generation) return lhs.generation < rhs.generation;
    return lhs.revision < rhs.revision;
  }

  // Ordering relation reported by compare(): lets callers distinguish "older" from "different".
  [[nodiscard]] std::string str() const;
};

enum class GenerationOrder : std::uint8_t {
  kOlder = 0,
  kEqual = 1,
  kNewer = 2,
  kIncomparable = 3,  // higher generation but lower epoch, or vice versa
};

[[nodiscard]] GenerationOrder compare_generation(const GenerationVector& candidate,
                                                 const GenerationVector& reference) noexcept;

// How much a source is trusted. Authority is explicit evidence metadata, never inferred from
// the source's name or transport.
enum class AuthorityLevel : std::uint8_t {
  kUnknown = 0,
  kUnverified = 1,   // source asserted a value but nothing corroborates it
  kReported = 2,     // source is a known reporting agent (agent/controller)
  kMeasured = 3,     // source measured the value at the object itself (device counter)
  kCorroborated = 4, // multiple independent measured sources agree
};

[[nodiscard]] std::string_view to_string(AuthorityLevel level) noexcept;
[[nodiscard]] Result<AuthorityLevel> authority_from_string(std::string_view text);

// The fence: everything needed to decide whether an incoming observation may influence current
// state, or whether it is a replay from a superseded epoch/generation/incarnation/sequence.
struct FenceVector {
  SourceId source{};
  BootId boot{};
  Incarnation incarnation{};
  GenerationVector gen{};
  Sequence sequence{};
};

enum class FenceDecision : std::uint8_t {
  kAcceptedFirstObservation = 0,
  kAcceptedInOrder,
  kAcceptedGapDetected,      // sequence jumped forward: accepted, gap recorded
  kAcceptedReboot,           // new boot id: accepted, previous incarnation retired
  kAcceptedEpochAdvance,
  kAcceptedGenerationAdvance,
  kRejectedUnknownSource,
  kRejectedStaleEpoch,
  kRejectedStaleGeneration,
  kRejectedStaleRevision,
  kRejectedIncomparableGeneration,
  kRejectedReplayedSequence,
  kRejectedStaleIncarnation,
  kRejectedAuthorityTooLow,
  kRejectedInconsistentBoot,
};

[[nodiscard]] std::string_view to_string(FenceDecision decision) noexcept;
[[nodiscard]] bool fence_accepted(FenceDecision decision) noexcept;

struct FenceOutcome {
  FenceDecision decision{FenceDecision::kRejectedUnknownSource};
  std::string detail{};
  bool liveness_reset{false};   // previous incarnation retired: old evidence is no longer live
  bool sequence_gap{false};     // accepted, but sequencing is incomplete
  std::uint64_t gap_size{0};

  [[nodiscard]] bool accepted() const noexcept { return fence_accepted(decision); }
};

// State a source has reached, as tracked by the fence table.
struct SourceFenceState {
  SourceId source{};
  FenceVector last{};
  Timestamp last_seen{};
  AuthorityLevel last_authority{AuthorityLevel::kUnknown};
  bool live{false};
  bool has_state{false};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
};

// Decides whether an observation fences in. The table state is supplied by the caller so that
// this function stays pure, deterministic and trivially testable.
[[nodiscard]] FenceOutcome evaluate_fence(const FenceVector& incoming, AuthorityLevel authority,
                                          AuthorityLevel minimum_authority,
                                          const SourceFenceState& previous) noexcept;

}  // namespace congestion

#endif  // CONGESTION_MODEL_GENERATION_HPP
