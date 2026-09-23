// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/correlation/group.hpp"

#include <algorithm>
#include <map>

namespace congestion {
namespace {

bool intervals_within(const Episode& lhs, const Episode& rhs, Duration max_gap) {
  if (lhs.first_seen <= rhs.last_seen && rhs.first_seen <= lhs.last_seen) {
    return true;  // overlapping
  }
  const Timestamp gap_start = lhs.first_seen > rhs.last_seen ? lhs.first_seen : rhs.first_seen;
  const Timestamp gap_end = lhs.first_seen > rhs.last_seen ? rhs.last_seen : lhs.last_seen;
  return (gap_start - gap_end) <= max_gap;
}

}  // namespace

std::string_view to_string(MergeDecisionCode code) noexcept {
  switch (code) {
    case MergeDecisionCode::kMergedSameScope: return "merged_same_scope";
    case MergeDecisionCode::kMergedAdjacentInterval: return "merged_adjacent_interval";
    case MergeDecisionCode::kMergedSharedSources: return "merged_shared_sources";
    case MergeDecisionCode::kRejectedGenerationMismatch: return "rejected_generation_mismatch";
    case MergeDecisionCode::kRejectedMechanismMismatch: return "rejected_mechanism_mismatch";
    case MergeDecisionCode::kRejectedWindowSeparated: return "rejected_window_separated";
    case MergeDecisionCode::kRejectedScopeKindMismatch: return "rejected_scope_kind_mismatch";
    case MergeDecisionCode::kRejectedTenantBoundary: return "rejected_tenant_boundary";
    case MergeDecisionCode::kRejectedGroupFull: return "rejected_group_full";
    case MergeDecisionCode::kRejectedPolicyVersionMismatch: return "rejected_policy_version_mismatch";
    case MergeDecisionCode::kRejectedGroupLimit: return "rejected_group_limit";
  }
  return "rejected_group_limit";
}

bool merge_accepted(MergeDecisionCode code) noexcept {
  switch (code) {
    case MergeDecisionCode::kMergedSameScope:
    case MergeDecisionCode::kMergedAdjacentInterval:
    case MergeDecisionCode::kMergedSharedSources:
      return true;
    default:
      return false;
  }
}

GroupId compute_group_id(const std::vector<EpisodeId>& members) noexcept {
  std::vector<EpisodeId> ordered = members;
  std::sort(ordered.begin(), ordered.end());
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
  StableHasher hasher;
  hasher.update("congestion-observatory/group/1");
  hasher.separator();
  for (const EpisodeId& member : ordered) {
    hasher.update(member.str());
    hasher.separator();
  }
  return GroupId::from_digest(hasher.digest128());
}

Result<std::vector<CorrelationGroup>> correlate(const std::vector<Episode>& episodes,
                                                const CorrelationPolicy& policy,
                                                const Limits& limits,
                                                const CancellationToken& token) {
  std::vector<Episode> ordered = episodes;
  std::sort(ordered.begin(), ordered.end(), [](const Episode& lhs, const Episode& rhs) {
    if (lhs.first_seen != rhs.first_seen) {
      return lhs.first_seen < rhs.first_seen;
    }
    return lhs.id < rhs.id;
  });

  struct WorkingGroup {
    std::vector<EpisodeId> members{};
    Timestamp window_start{};
    Timestamp window_end{};
    std::vector<MergeDecision> decisions{};
    std::vector<Episode> episodes{};
    bool size_capped{false};
    bool ambiguous_membership{false};
  };

  std::vector<WorkingGroup> groups;
  const std::size_t member_cap =
      std::min(policy.max_group_members, limits.max_group_members);

  for (const Episode& episode : ordered) {
    if (token.cancelled()) {
      return make_error(ErrorCode::kCancelled, "correlation cancelled");
    }
    bool placed = false;
    for (WorkingGroup& group : groups) {
      if (token.cancelled()) {
        return make_error(ErrorCode::kCancelled, "correlation cancelled");
      }
      const Episode& reference = group.episodes.front();
      MergeDecisionCode code = MergeDecisionCode::kMergedSameScope;
      std::string detail;

      if (policy.require_same_generation && episode.key.generation != reference.key.generation) {
        code = MergeDecisionCode::kRejectedGenerationMismatch;
        detail = episode.key.generation.str() + " != " + reference.key.generation.str();
      } else if (policy.require_same_mechanism &&
                 episode.key.mechanism != reference.key.mechanism) {
        code = MergeDecisionCode::kRejectedMechanismMismatch;
        detail = std::string(to_string(episode.key.mechanism)) + " != " +
                 std::string(to_string(reference.key.mechanism));
      } else if (episode.key.policy_version != reference.key.policy_version) {
        code = MergeDecisionCode::kRejectedPolicyVersionMismatch;
        detail = episode.key.policy_version + " != " + reference.key.policy_version;
      } else if (!policy.merge_across_scope_kinds &&
                 episode.key.scope.kind() != reference.key.scope.kind()) {
        code = MergeDecisionCode::kRejectedScopeKindMismatch;
        detail = std::string(to_string(episode.key.scope.kind())) + " != " +
                 std::string(to_string(reference.key.scope.kind()));
      } else if (!policy.merge_across_tenants && episode.key.tenant.valid() &&
                 reference.key.tenant.valid() && episode.key.tenant != reference.key.tenant) {
        code = MergeDecisionCode::kRejectedTenantBoundary;
        detail = episode.key.tenant.str() + " != " + reference.key.tenant.str();
      } else if (group.members.size() >= member_cap) {
        code = MergeDecisionCode::kRejectedGroupFull;
        detail = "group member cap reached";
        group.size_capped = true;
      } else if (!intervals_within(episode, reference, policy.max_gap)) {
        code = MergeDecisionCode::kRejectedWindowSeparated;
        detail = "observed intervals are further apart than max_gap";
      } else if (episode.key.scope == reference.key.scope) {
        code = MergeDecisionCode::kMergedSameScope;
      } else {
        code = MergeDecisionCode::kMergedAdjacentInterval;
      }

      group.decisions.push_back(MergeDecision{episode.id, reference.id, code,
                                               merge_accepted(code), detail});
      if (!merge_accepted(code)) {
        if (code == MergeDecisionCode::kRejectedWindowSeparated ||
            code == MergeDecisionCode::kRejectedGroupFull) {
          group.ambiguous_membership = true;
        }
        continue;
      }
      group.members.push_back(episode.id);
      group.episodes.push_back(episode);
      if (episode.first_seen < group.window_start || group.window_start.is_zero()) {
        group.window_start = episode.first_seen;
      }
      if (episode.last_seen > group.window_end) {
        group.window_end = episode.last_seen;
      }
      placed = true;
      break;
    }

    if (placed) {
      continue;
    }
    if (groups.size() >= std::min(policy.max_groups, limits.max_groups)) {
      // No new group may be created: the episode stays ungrouped and says so.
      continue;
    }
    WorkingGroup group;
    group.members.push_back(episode.id);
    group.episodes.push_back(episode);
    group.window_start = episode.first_seen;
    group.window_end = episode.last_seen;
    group.decisions.push_back(MergeDecision{episode.id, episode.id,
                                            MergeDecisionCode::kMergedSameScope, true,
                                            "group opened"});
    groups.push_back(std::move(group));
  }

  std::vector<CorrelationGroup> out;
  out.reserve(groups.size());
  for (WorkingGroup& group : groups) {
    CorrelationGroup result;
    std::sort(group.members.begin(), group.members.end());
    group.members.erase(std::unique(group.members.begin(), group.members.end()),
                        group.members.end());
    result.members = group.members;
    result.id = compute_group_id(group.members);
    result.window_start = group.window_start;
    result.window_end = group.window_end;
    result.mechanism = group.episodes.front().key.mechanism;
    result.generation = group.episodes.front().key.generation;
    result.size_capped = group.size_capped;
    result.ambiguous_membership = group.ambiguous_membership;
    for (const Episode& member : group.episodes) {
      if (member.peak_severity > result.peak_severity) {
        result.peak_severity = member.peak_severity;
      }
    }
    result.decisions = group.decisions;
    std::sort(result.decisions.begin(), result.decisions.end(),
              [](const MergeDecision& lhs, const MergeDecision& rhs) {
                if (lhs.member != rhs.member) {
                  return lhs.member < rhs.member;
                }
                return lhs.reference < rhs.reference;
              });
    std::map<Name, SourceId> sources;
    for (const Episode& member : group.episodes) {
      for (const SourceId& source : member.sources) {
        sources.emplace(source.name(), source);
      }
    }
    result.sources.reserve(sources.size());
    for (const auto& entry : sources) {
      result.sources.push_back(entry.second);
    }
    out.push_back(std::move(result));
  }

  std::sort(out.begin(), out.end(), [](const CorrelationGroup& lhs, const CorrelationGroup& rhs) {
    if (lhs.window_start != rhs.window_start) {
      return lhs.window_start < rhs.window_start;
    }
    return lhs.id < rhs.id;
  });
  return out;
}

}  // namespace congestion
