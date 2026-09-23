// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/episode/episode.hpp"

#include <algorithm>

namespace congestion {

bool operator==(const EpisodeKey& lhs, const EpisodeKey& rhs) noexcept {
  return lhs.scope == rhs.scope && lhs.tenant == rhs.tenant && lhs.mechanism == rhs.mechanism &&
         lhs.generation == rhs.generation && lhs.policy_version == rhs.policy_version;
}

bool operator<(const EpisodeKey& lhs, const EpisodeKey& rhs) noexcept {
  if (lhs.scope != rhs.scope) return lhs.scope < rhs.scope;
  if (lhs.tenant != rhs.tenant) return lhs.tenant < rhs.tenant;
  if (lhs.mechanism != rhs.mechanism) return lhs.mechanism < rhs.mechanism;
  if (lhs.generation != rhs.generation) return lhs.generation < rhs.generation;
  return lhs.policy_version < rhs.policy_version;
}

std::string EpisodeKey::str() const {
  std::string out = scope.str();
  out.push_back('/');
  out += tenant.valid() ? tenant.str() : std::string("-");
  out.push_back('/');
  out += to_string(mechanism);
  out.push_back('/');
  out += generation.str();
  out.push_back('/');
  out += policy_version;
  return out;
}

void encode_episode_identity(const EpisodeKey& key, StableHasher& hasher) noexcept {
  hasher.update("congestion-observatory/episode/1");
  hasher.separator();
  hasher.update_u64(static_cast<std::uint64_t>(key.scope.kind()));
  hasher.update(key.scope.str());
  hasher.separator();
  hasher.update(key.tenant.view());
  hasher.separator();
  hasher.update_u64(static_cast<std::uint64_t>(key.mechanism));
  hasher.update_u64(key.generation.epoch.value());
  hasher.update_u64(key.generation.generation.value());
  hasher.update_u64(key.generation.revision.value());
  hasher.update(key.policy_version);
}

EpisodeId compute_episode_id(const EpisodeKey& key) noexcept {
  StableHasher hasher;
  encode_episode_identity(key, hasher);
  return EpisodeId::from_digest(hasher.digest128());
}

std::string_view to_string(EpisodeState state) noexcept {
  switch (state) {
    case EpisodeState::kOpen: return "open";
    case EpisodeState::kQuiescent: return "quiescent";
    case EpisodeState::kResolved: return "resolved";
    case EpisodeState::kExpired: return "expired";
  }
  return "expired";
}

std::string_view to_string(EpisodeTransitionKind kind) noexcept {
  switch (kind) {
    case EpisodeTransitionKind::kOpened: return "opened";
    case EpisodeTransitionKind::kUpdated: return "updated";
    case EpisodeTransitionKind::kEscalated: return "escalated";
    case EpisodeTransitionKind::kDeescalated: return "deescalated";
    case EpisodeTransitionKind::kQuiesced: return "quiesced";
    case EpisodeTransitionKind::kResolved: return "resolved";
    case EpisodeTransitionKind::kExpired: return "expired";
    case EpisodeTransitionKind::kReopened: return "reopened";
  }
  return "updated";
}

std::string_view to_string(EpisodeUpdateKind kind) noexcept {
  switch (kind) {
    case EpisodeUpdateKind::kOpened: return "opened";
    case EpisodeUpdateKind::kUpdated: return "updated";
    case EpisodeUpdateKind::kEscalated: return "escalated";
    case EpisodeUpdateKind::kDeescalated: return "deescalated";
    case EpisodeUpdateKind::kQuiesced: return "quiesced";
    case EpisodeUpdateKind::kResolved: return "resolved";
    case EpisodeUpdateKind::kExpired: return "expired";
    case EpisodeUpdateKind::kUnchanged: return "unchanged";
  }
  return "unchanged";
}

namespace {

AssessmentDigest digest_of(const CongestionAssessment& assessment) {
  AssessmentDigest digest;
  digest.at = assessment.evaluated_at;
  digest.verdict = assessment.verdict;
  digest.mechanisms = assessment.mechanisms;
  digest.severity = assessment.severity;
  digest.confidence = assessment.confidence;
  digest.citation_count = assessment.citations.size();
  digest.fresh_confirming_evidence = assessment.congestion_asserted;
  if (!assessment.citations.empty()) {
    digest.representative_citation = assessment.citations.front();
  }
  return digest;
}

void append_transition(Episode& episode, EpisodeTransition transition, const Limits& limits) {
  episode.transitions.push_back(std::move(transition));
  while (episode.transitions.size() > limits.max_episode_transitions) {
    episode.transitions.erase(episode.transitions.begin());
    ++episode.transitions_dropped;
    episode.history_truncated = true;
  }
}

void append_assessment(Episode& episode, AssessmentDigest digest, const Limits& limits) {
  episode.assessments.push_back(std::move(digest));
  while (episode.assessments.size() > limits.max_episode_assessments) {
    episode.assessments.erase(episode.assessments.begin());
    ++episode.assessments_dropped;
    episode.history_truncated = true;
  }
}

}  // namespace

EpisodeRegistry::EpisodeRegistry(const Limits& limits) : limits_(limits) {}

Result<EpisodeUpdate> EpisodeRegistry::observe(const CongestionAssessment& assessment,
                                               const EpisodePolicy& policy, Timestamp now) {
  const std::lock_guard<std::mutex> lock(mutex_);
  return observe_locked(assessment, policy, now);
}

Result<EpisodeUpdate> EpisodeRegistry::observe_locked(const CongestionAssessment& assessment,
                                                      const EpisodePolicy& policy, Timestamp now) {
  (void)policy;
  EpisodeUpdate update;
  if (!assessment.subject.valid()) {
    return make_error(ErrorCode::kInvalidArgument, "episode observation requires a subject");
  }

  if (!assessment.congestion_asserted) {
    // Congestion is not asserted for this scope: any open episode for the scope becomes
    // quiescent. Only the state change is recorded, never a verdict.
    EpisodeUpdate first;
    bool any = false;
    for (auto& entry : episodes_) {
      Episode& episode = entry.second;
      if (episode.key.scope != assessment.subject) {
        continue;
      }
      if (episode.state != EpisodeState::kOpen) {
        continue;
      }
      const Severity previous_severity = episode.current_severity;
      const Verdict previous_verdict =
          episode.assessments.empty() ? Verdict::kNoEvidence
                                      : episode.assessments.back().verdict;
      episode.state = EpisodeState::kQuiescent;
      episode.current_severity = Severity::kNone;
      episode.last_updated = now;
      ++episode.revision;
      episode.observations += 1;
      EpisodeTransition transition;
      transition.revision = episode.revision;
      transition.kind = EpisodeTransitionKind::kQuiesced;
      transition.at = now;
      transition.severity_before = previous_severity;
      transition.severity_after = Severity::kNone;
      transition.verdict_before = previous_verdict;
      transition.verdict_after = assessment.verdict;
      transition.confidence = assessment.confidence;
      transition.cause = "verdict_without_congestion_assertion";
      transition.citations = assessment.citations;
      append_transition(episode, std::move(transition), limits_);
      append_assessment(episode, digest_of(assessment), limits_);
      if (!any) {
        first.id = episode.id;
        first.kind = EpisodeUpdateKind::kQuiesced;
        first.state = episode.state;
        first.revision = episode.revision;
        first.severity = episode.current_severity;
        any = true;
      }
    }
    if (!any) {
      update.kind = EpisodeUpdateKind::kUnchanged;
    } else {
      update = first;
    }
    return update;
  }

  const Mechanism mechanism = primary_mechanism(assessment.mechanisms);
  if (mechanism == Mechanism::kNone) {
    return make_error(ErrorCode::kPreconditionFailed,
                      "a congestion assertion must name at least one mechanism",
                      assessment.subject.str());
  }

  EpisodeKey key;
  key.scope = assessment.subject;
  key.tenant = assessment.tenant;
  key.mechanism = mechanism;
  key.generation = assessment.generation;
  key.policy_version = assessment.explanation.policy_version;
  const EpisodeId id = compute_episode_id(key);

  const auto existing = episodes_.find(id);
  if (existing == episodes_.end()) {
    if (episodes_.size() >= limits_.max_episodes) {
      // Evict the oldest episode that reached a terminal state. If every episode is still live the
      // new one is refused rather than silently displacing an active investigation.
      auto victim = episodes_.end();
      for (auto it = episodes_.begin(); it != episodes_.end(); ++it) {
        if (it->second.state == EpisodeState::kResolved || it->second.state == EpisodeState::kExpired) {
          if (victim == episodes_.end() || it->second.last_updated < victim->second.last_updated) {
            victim = it;
          }
        }
      }
      if (victim == episodes_.end()) {
        ++dropped_episodes_;
        return make_error(ErrorCode::kLimitExceeded,
                          "episode budget exhausted and no terminal episode can be retired",
                          assessment.subject.str());
      }
      episodes_.erase(victim);
      ++dropped_episodes_;
    }

    Episode episode;
    episode.id = id;
    episode.key = key;
    episode.state = EpisodeState::kOpen;
    episode.revision = Revision(1);
    episode.current_severity = assessment.severity;
    episode.peak_severity = assessment.severity;
    episode.confidence = assessment.confidence;
    episode.first_seen = now;
    episode.last_seen = now;
    episode.last_updated = now;
    episode.observations = 1;
    episode.citations = assessment.citations;
    episode.sources = assessment.sources;
    EpisodeTransition transition;
    transition.revision = episode.revision;
    transition.kind = EpisodeTransitionKind::kOpened;
    transition.at = now;
    transition.severity_after = assessment.severity;
    transition.verdict_after = assessment.verdict;
    transition.confidence = assessment.confidence;
    transition.cause = "congestion_asserted";
    transition.citations = assessment.citations;
    append_transition(episode, std::move(transition), limits_);
    append_assessment(episode, digest_of(assessment), limits_);
    episodes_.emplace(id, std::move(episode));

    update.id = id;
    update.kind = EpisodeUpdateKind::kOpened;
    update.state = EpisodeState::kOpen;
    update.revision = Revision(1);
    update.severity = assessment.severity;
    update.created = true;
    return update;
  }

  Episode& episode = existing->second;
  const Severity previous_severity = episode.current_severity;
  const Verdict previous_verdict = episode.assessments.empty() ? Verdict::kNoEvidence
                                                               : episode.assessments.back().verdict;
  const bool reopened = episode.state == EpisodeState::kResolved || episode.state == EpisodeState::kExpired;

  episode.state = EpisodeState::kOpen;
  episode.current_severity = assessment.severity;
  if (assessment.severity > episode.peak_severity) {
    episode.peak_severity = assessment.severity;
  }
  episode.confidence = assessment.confidence;
  episode.last_seen = now;
  episode.last_updated = now;
  episode.observations += 1;
  episode.citations = assessment.citations;
  if (!assessment.sources.empty()) {
    episode.sources = assessment.sources;
  }
  ++episode.revision;

  EpisodeTransitionKind kind = EpisodeTransitionKind::kUpdated;
  if (reopened) {
    kind = EpisodeTransitionKind::kReopened;
  } else if (assessment.severity > previous_severity) {
    kind = EpisodeTransitionKind::kEscalated;
  } else if (assessment.severity < previous_severity) {
    kind = EpisodeTransitionKind::kDeescalated;
  }

  EpisodeTransition transition;
  transition.revision = episode.revision;
  transition.kind = kind;
  transition.at = now;
  transition.severity_before = previous_severity;
  transition.severity_after = assessment.severity;
  transition.verdict_before = previous_verdict;
  transition.verdict_after = assessment.verdict;
  transition.confidence = assessment.confidence;
  transition.cause = reopened ? "reopened_by_fresh_evidence" : "refreshed_by_fresh_evidence";
  transition.citations = assessment.citations;
  append_transition(episode, std::move(transition), limits_);
  append_assessment(episode, digest_of(assessment), limits_);

  update.id = id;
  update.state = episode.state;
  update.revision = episode.revision;
  update.severity = episode.current_severity;
  switch (kind) {
    case EpisodeTransitionKind::kEscalated: update.kind = EpisodeUpdateKind::kEscalated; break;
    case EpisodeTransitionKind::kDeescalated: update.kind = EpisodeUpdateKind::kDeescalated; break;
    case EpisodeTransitionKind::kReopened: update.kind = EpisodeUpdateKind::kOpened; break;
    default: update.kind = EpisodeUpdateKind::kUpdated; break;
  }
  return update;
}

Result<std::vector<EpisodeUpdate>> EpisodeRegistry::advance(Timestamp now,
                                                            const EpisodePolicy& policy,
                                                            const CancellationToken& token) {
  std::vector<EpisodeUpdate> updates;
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : episodes_) {
    if (token.cancelled()) {
      return make_error(ErrorCode::kCancelled, "episode advance cancelled");
    }
    Episode& episode = entry.second;
    const Duration idle = now - episode.last_seen;
    if (idle.is_negative()) {
      continue;
    }
    EpisodeTransitionKind kind = EpisodeTransitionKind::kUpdated;
    EpisodeState next_state = episode.state;
    if (episode.state == EpisodeState::kOpen && idle > policy.resolve_after) {
      next_state = EpisodeState::kQuiescent;
      kind = EpisodeTransitionKind::kQuiesced;
    } else if (episode.state == EpisodeState::kQuiescent &&
               idle > Duration(policy.resolve_after.nanos() * 2)) {
      next_state = EpisodeState::kResolved;
      kind = EpisodeTransitionKind::kResolved;
    } else if ((episode.state == EpisodeState::kResolved ||
                episode.state == EpisodeState::kQuiescent) &&
               idle > policy.expire_after) {
      next_state = EpisodeState::kExpired;
      kind = EpisodeTransitionKind::kExpired;
    } else if (episode.state == EpisodeState::kExpired) {
      continue;
    } else {
      continue;
    }

    const Severity previous_severity = episode.current_severity;
    episode.state = next_state;
    episode.last_updated = now;
    ++episode.revision;
    if (next_state == EpisodeState::kResolved || next_state == EpisodeState::kExpired) {
      episode.resolved_at = now;
      episode.current_severity = Severity::kNone;
    }

    EpisodeTransition transition;
    transition.revision = episode.revision;
    transition.kind = kind;
    transition.at = now;
    transition.severity_before = previous_severity;
    transition.severity_after = episode.current_severity;
    transition.verdict_before = episode.assessments.empty() ? Verdict::kNoEvidence
                                                            : episode.assessments.back().verdict;
    transition.verdict_after = transition.verdict_before;
    transition.confidence = episode.confidence;
    transition.cause = "policy_window_elapsed";
    append_transition(episode, std::move(transition), limits_);

    EpisodeUpdate update;
    update.id = episode.id;
    update.state = episode.state;
    update.revision = episode.revision;
    update.severity = episode.current_severity;
    switch (kind) {
      case EpisodeTransitionKind::kQuiesced: update.kind = EpisodeUpdateKind::kQuiesced; break;
      case EpisodeTransitionKind::kResolved: update.kind = EpisodeUpdateKind::kResolved; break;
      case EpisodeTransitionKind::kExpired: update.kind = EpisodeUpdateKind::kExpired; break;
      default: update.kind = EpisodeUpdateKind::kUpdated; break;
    }
    updates.push_back(update);
  }
  return updates;
}

std::vector<Episode> EpisodeRegistry::list(const EpisodeFilter& filter) const {
  std::vector<Episode> out;
  const std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t budget = std::min(filter.max_results, limits_.max_episode_filter_results);
  for (const auto& entry : episodes_) {
    if (out.size() >= budget) {
      break;
    }
    const Episode& episode = entry.second;
    if (filter.scope != nullptr && episode.key.scope != *filter.scope) {
      continue;
    }
    if (!filter.include_closed && (episode.state == EpisodeState::kResolved ||
                                   episode.state == EpisodeState::kExpired)) {
      continue;
    }
    out.push_back(episode);
  }
  std::sort(out.begin(), out.end(), [](const Episode& lhs, const Episode& rhs) {
    if (lhs.first_seen != rhs.first_seen) {
      return lhs.first_seen < rhs.first_seen;
    }
    return lhs.id < rhs.id;
  });
  return out;
}

Result<Episode> EpisodeRegistry::get(const EpisodeId& id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = episodes_.find(id);
  if (it == episodes_.end()) {
    return make_error(ErrorCode::kNotFound, "episode is not known", id.str());
  }
  return it->second;
}

std::size_t EpisodeRegistry::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return episodes_.size();
}

std::vector<Episode> EpisodeRegistry::all() const {
  std::vector<Episode> out;
  const std::lock_guard<std::mutex> lock(mutex_);
  out.reserve(episodes_.size());
  for (const auto& entry : episodes_) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(), [](const Episode& lhs, const Episode& rhs) {
    if (lhs.first_seen != rhs.first_seen) {
      return lhs.first_seen < rhs.first_seen;
    }
    return lhs.id < rhs.id;
  });
  return out;
}

std::uint64_t EpisodeRegistry::dropped_episodes() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return dropped_episodes_;
}

Status EpisodeRegistry::restore(const Episode& episode) {
  if (!episode.id.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "restored episode has no identity"));
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  auto it = episodes_.find(episode.id);
  if (it != episodes_.end()) {
    if (it->second.revision > episode.revision) {
      return Status(make_error(ErrorCode::kConflict,
                               "a newer revision of this episode is already loaded",
                               episode.id.str()));
    }
    it->second = episode;
    return Status{};
  }
  if (episodes_.size() >= limits_.max_episodes) {
    return Status(make_error(ErrorCode::kLimitExceeded, "episode budget exhausted while restoring",
                             episode.id.str()));
  }
  episodes_.emplace(episode.id, episode);
  return Status{};
}

}  // namespace congestion
