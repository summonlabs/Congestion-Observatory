// Congestion Observatory - causal localization with explicit ambiguity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_LOCALIZATION_LOCALIZE_HPP
#define CONGESTION_LOCALIZATION_LOCALIZE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "congestion/assessment/classify.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/evidence/evidence.hpp"
#include "congestion/localization/graph.hpp"

namespace congestion {

struct LocalizationPolicy {
  std::size_t max_depth{6};
  std::size_t max_candidates{8};
  // Two candidates whose scores differ by less than this margin are reported as ambiguous
  // instead of being ranked apart.
  std::uint32_t ambiguity_margin{5};
  std::uint32_t minimum_candidate_score{10};
  bool require_decisive_evidence{true};
  Duration evidence_window{Duration::from_seconds(10)};
  bool allow_structural_only_edges{false};
};

enum class LocalizationOutcome : std::uint8_t {
  kLocalized = 0,    // exactly one candidate dominates
  kAmbiguous = 1,    // several candidates are indistinguishable; all are reported
  kUnlocalized = 2,  // no candidate reached the minimum score
  kNotAttempted = 3, // the symptom is not a congestion assertion
};

[[nodiscard]] std::string_view to_string(LocalizationOutcome outcome) noexcept;

struct LocalizationCandidate {
  EvidenceSubject subject{};
  std::uint32_t score{0};
  std::uint32_t confidence{0};  // 0..100
  std::size_t depth{0};
  std::vector<EvidenceId> citations{};
  std::vector<std::string> rules{};   // edge rule ids traversed, in order
  std::vector<std::string> basis{};   // named contributions to the score
};

struct LocalizationRequest {
  EvidenceSubject symptom{};
  Timestamp window_start{};
  Timestamp window_end{};
  Timestamp evaluated_at{};
  GenerationVector generation{};
  const CongestionAssessment* assessment{nullptr};
};

struct LocalizationResult {
  EvidenceSubject symptom{};
  LocalizationOutcome outcome{LocalizationOutcome::kNotAttempted};
  std::vector<LocalizationCandidate> candidates{};
  std::vector<Blocker> blockers{};
  Explanation explanation{};
  std::size_t paths_examined{0};
  bool depth_limited{false};
};

// Localizes the origin of an asserted congestion symptom by walking evidence-backed causal edges
// away from the symptom. A candidate is only reported when its own evidence is decisive;
// structural adjacency alone never produces a culprit.
[[nodiscard]] Result<LocalizationResult> localize(
    const LocalizationRequest& request, const CausalGraph& graph,
    const std::vector<const EvidenceRecord*>& records, const ClassificationPolicy& policy,
    const LocalizationPolicy& localization_policy, const Limits& limits);

namespace localization_blockers {
inline constexpr std::string_view kNoEvidenceBackedPath = "no_evidence_backed_causal_path";
inline constexpr std::string_view kAmbiguousCandidates = "ambiguous_multiple_candidates";
inline constexpr std::string_view kSymptomNotAsserted = "symptom_not_asserted_as_congestion";
inline constexpr std::string_view kDepthLimited = "search_depth_limited";
inline constexpr std::string_view kCandidateEvidenceStale = "candidate_evidence_not_fresh";
inline constexpr std::string_view kStructuralOnlyRejected = "structural_edge_without_evidence";
inline constexpr std::string_view kGraphEmpty = "causal_graph_empty";
}  // namespace localization_blockers

}  // namespace congestion

#endif  // CONGESTION_LOCALIZATION_LOCALIZE_HPP
