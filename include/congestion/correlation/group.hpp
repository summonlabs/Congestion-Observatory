// Congestion Observatory - deterministic episode correlation without over-merging.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORRELATION_GROUP_HPP
#define CONGESTION_CORRELATION_GROUP_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "congestion/core/cancel.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/episode/episode.hpp"

namespace congestion {

struct CorrelationPolicy {
  // Two episodes may only be grouped when their observed intervals are within this span.
  Duration window_span{Duration::from_seconds(30)};
  // Absolute gap allowed between the end of one episode and the start of the next.
  Duration max_gap{Duration::from_seconds(10)};
  bool require_same_mechanism{true};
  bool require_same_generation{true};
  // Merging across tenants is off by default: distinct tenants usually mean distinct causes.
  bool merge_across_tenants{false};
  bool merge_across_scope_kinds{false};
  std::size_t max_group_members{64};
  std::size_t max_groups{1024};
};

enum class MergeDecisionCode : std::uint8_t {
  kMergedSameScope = 0,
  kMergedAdjacentInterval = 1,
  kMergedSharedSources = 2,
  kRejectedGenerationMismatch = 3,
  kRejectedMechanismMismatch = 4,
  kRejectedWindowSeparated = 5,
  kRejectedScopeKindMismatch = 6,
  kRejectedTenantBoundary = 7,
  kRejectedGroupFull = 8,
  kRejectedPolicyVersionMismatch = 9,
  kRejectedGroupLimit = 10,
};

[[nodiscard]] std::string_view to_string(MergeDecisionCode code) noexcept;
[[nodiscard]] bool merge_accepted(MergeDecisionCode code) noexcept;

struct MergeDecision {
  EpisodeId member{};
  EpisodeId reference{};
  MergeDecisionCode code{MergeDecisionCode::kMergedSameScope};
  bool merged{false};
  std::string detail{};
};

struct CorrelationGroup {
  GroupId id{};
  std::vector<EpisodeId> members{};  // ascending
  Timestamp window_start{};
  Timestamp window_end{};
  Mechanism mechanism{Mechanism::kNone};
  GenerationVector generation{};
  std::vector<SourceId> sources{};  // ascending, deduplicated
  std::vector<MergeDecision> decisions{};
  Severity peak_severity{Severity::kNone};
  bool size_capped{false};
  bool ambiguous_membership{false};  // at least one member was rejected on a boundary rule
};

// Groups episodes that share a cause hypothesis. Grouping is deterministic: inputs are ordered
// by (first_seen, id), members are compared against a stable reference, every accept and reject
// is recorded, and the group id is derived from the sorted member ids.
[[nodiscard]] Result<std::vector<CorrelationGroup>> correlate(const std::vector<Episode>& episodes,
                                                              const CorrelationPolicy& policy,
                                                              const Limits& limits,
                                                              const CancellationToken& token);

// Derives the group id from the member set. Exposed so that callers can verify determinism.
[[nodiscard]] GroupId compute_group_id(const std::vector<EpisodeId>& members) noexcept;

}  // namespace congestion

#endif  // CONGESTION_CORRELATION_GROUP_HPP
