// Congestion Observatory - deterministic episode identity, lifecycle and history.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_EPISODE_EPISODE_HPP
#define CONGESTION_EPISODE_EPISODE_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "congestion/assessment/classify.hpp"
#include "congestion/core/cancel.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/model/generation.hpp"
#include "congestion/model/identities.hpp"

namespace congestion {

// Episode identity is the tuple (scope, mechanism, generation, policy). It is deliberately NOT a
// function of wall clock: the same phenomenon observed by two collectors at different times still
// yields the same episode id, while a generation change yields a new identity so that a stale
// replay can never resurrect or extend a retired episode.
struct EpisodeKey {
  EvidenceSubject scope{};
  TenantId tenant{};
  Mechanism mechanism{Mechanism::kNone};
  GenerationVector generation{};
  std::string policy_version{};

  friend bool operator==(const EpisodeKey& lhs, const EpisodeKey& rhs) noexcept;
  friend bool operator<(const EpisodeKey& lhs, const EpisodeKey& rhs) noexcept;
  [[nodiscard]] std::string str() const;
};

[[nodiscard]] EpisodeId compute_episode_id(const EpisodeKey& key) noexcept;
void encode_episode_identity(const EpisodeKey& key, StableHasher& hasher) noexcept;

enum class EpisodeState : std::uint8_t {
  kOpen = 0,       // congestion asserted, still receiving fresh confirming evidence
  kQuiescent = 1,  // no fresh confirming evidence, not yet resolvable
  kResolved = 2,   // congestion ended and the resolution window elapsed
  kExpired = 3,    // no further evidence and the expiry window elapsed
};

[[nodiscard]] std::string_view to_string(EpisodeState state) noexcept;

enum class EpisodeTransitionKind : std::uint8_t {
  kOpened = 0,
  kUpdated = 1,
  kEscalated = 2,
  kDeescalated = 3,
  kQuiesced = 4,
  kResolved = 5,
  kExpired = 6,
  kReopened = 7,
};

[[nodiscard]] std::string_view to_string(EpisodeTransitionKind kind) noexcept;

struct AssessmentDigest {
  Timestamp at{};
  Verdict verdict{Verdict::kNoEvidence};
  MechanismSet mechanisms{0};
  Severity severity{Severity::kNone};
  std::uint32_t confidence{0};
  std::size_t citation_count{0};
  bool fresh_confirming_evidence{false};
  EvidenceId representative_citation{};
};

struct EpisodeTransition {
  Revision revision{};
  EpisodeTransitionKind kind{EpisodeTransitionKind::kOpened};
  Timestamp at{};
  Severity severity_before{Severity::kNone};
  Severity severity_after{Severity::kNone};
  Verdict verdict_before{Verdict::kNoEvidence};
  Verdict verdict_after{Verdict::kNoEvidence};
  std::uint32_t confidence{0};
  std::string cause{};
  std::vector<EvidenceId> citations{};
};

struct Episode {
  EpisodeId id{};
  EpisodeKey key{};  // includes scope, tenant, mechanism, generation and policy version
  EpisodeState state{EpisodeState::kOpen};
  Revision revision{};
  Severity current_severity{Severity::kNone};
  Severity peak_severity{Severity::kNone};
  std::uint32_t confidence{0};
  Timestamp first_seen{};
  Timestamp last_seen{};
  Timestamp last_updated{};
  Timestamp resolved_at{};
  std::uint64_t observations{0};
  std::vector<EpisodeTransition> transitions{};  // oldest first, bounded
  std::vector<AssessmentDigest> assessments{};   // oldest first, bounded
  std::vector<EvidenceId> citations{};           // most recent deciding citations
  std::vector<SourceId> sources{};               // sources that confirmed this episode
  std::uint64_t transitions_dropped{0};          // dropped from the front, never silently
  std::uint64_t assessments_dropped{0};
  bool history_truncated{false};
};

struct EpisodePolicy {
  Duration resolve_after{Duration::from_seconds(30)};
  Duration expire_after{Duration::from_minutes(10)};
  Severity escalate_at{Severity::kHigh};
};

enum class EpisodeUpdateKind : std::uint8_t {
  kOpened = 0,
  kUpdated = 1,
  kEscalated = 2,
  kDeescalated = 3,
  kQuiesced = 4,
  kResolved = 5,
  kExpired = 6,
  kUnchanged = 7,
};

[[nodiscard]] std::string_view to_string(EpisodeUpdateKind kind) noexcept;

struct EpisodeUpdate {
  EpisodeId id{};
  EpisodeUpdateKind kind{EpisodeUpdateKind::kUnchanged};
  EpisodeState state{EpisodeState::kOpen};
  Revision revision{};
  Severity severity{Severity::kNone};
  bool created{false};
};

struct EpisodeFilter {
  const EvidenceSubject* scope{nullptr};
  bool include_closed{true};
  std::size_t max_results{256};
};

// Bounded, deterministic episode registry. Every mutation is expressed as an observation of a
// classification result; identical observation sequences always produce the same history.
class EpisodeRegistry {
 public:
  explicit EpisodeRegistry(const Limits& limits);

  // Records a classification result. Only congestion assertions open or refresh an episode: a
  // verdict that merely reports utilization never creates congestion history.
  [[nodiscard]] Result<EpisodeUpdate> observe(const CongestionAssessment& assessment,
                                              const EpisodePolicy& policy, Timestamp now);

  // Applies the quiescence/resolution/expiry windows. Deterministic given (now, policy).
  [[nodiscard]] Result<std::vector<EpisodeUpdate>> advance(Timestamp now,
                                                           const EpisodePolicy& policy,
                                                           const CancellationToken& token);

  [[nodiscard]] std::vector<Episode> list(const EpisodeFilter& filter) const;
  [[nodiscard]] Result<Episode> get(const EpisodeId& id) const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::vector<Episode> all() const;
  [[nodiscard]] std::uint64_t dropped_episodes() const;

  // Restores an episode from a snapshot, preserving revision and history. Restored episodes carry
  // no liveness: they are reopened only by fresh evidence.
  [[nodiscard]] Status restore(const Episode& episode);

 private:
  [[nodiscard]] Result<EpisodeUpdate> observe_locked(const CongestionAssessment& assessment,
                                                     const EpisodePolicy& policy, Timestamp now);

  Limits limits_;
  mutable std::mutex mutex_;
  std::map<EpisodeId, Episode> episodes_{};
  std::uint64_t dropped_episodes_{0};
};

}  // namespace congestion

#endif  // CONGESTION_EPISODE_EPISODE_HPP
