// Congestion Observatory - congestion classification with explicit distinctions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_ASSESSMENT_CLASSIFY_HPP
#define CONGESTION_ASSESSMENT_CLASSIFY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "congestion/assessment/policy.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/evidence/evidence.hpp"
#include "congestion/model/generation.hpp"

namespace congestion {

// The verdict taxonomy keeps utilization, contention, pressure, saturation, congestion and
// indeterminacy strictly apart. In particular kUtilizedHealthy is a *first class positive
// finding*: the resource is busy and nothing shows harm.
enum class Verdict : std::uint8_t {
  kNoEvidence = 0,          // nothing was observed for this subject in the window
  kIdle = 1,                // capacity is available and demand is low
  kUtilizedHealthy = 2,     // high utilization, no impairment evidence: NOT congestion
  kContentionObserved = 3,  // multiple consumers compete; no harm demonstrated yet
  kPressureObserved = 4,    // a backlog exists; no harm demonstrated yet
  kSaturated = 5,           // offered demand meets or exceeds serviceable capacity
  kCongestionConfirmed = 6, // impairment evidence is present and fresh
  kIndeterminate = 7,       // evidence is stale, conflicting, incomplete, unsupported or unknown
  kCount = 8,
};

[[nodiscard]] std::string_view to_string(Verdict verdict) noexcept;
// True only for verdicts that assert observed harm.
[[nodiscard]] bool is_congestion_assertion(Verdict verdict) noexcept;

enum class Mechanism : std::uint8_t {
  kNone = 0,
  kPacketDrop = 1,
  kEcnMarking = 2,
  kPauseBackpressure = 3,
  kLatencyInflation = 4,
  kRetransmission = 5,
  kCount = 6,
};

using MechanismSet = std::uint32_t;

[[nodiscard]] constexpr MechanismSet mechanism_bit(Mechanism mechanism) noexcept {
  return static_cast<MechanismSet>(1u) << static_cast<unsigned>(mechanism);
}
[[nodiscard]] std::string_view to_string(Mechanism mechanism) noexcept;
[[nodiscard]] bool has_mechanism(MechanismSet set, Mechanism mechanism) noexcept;
[[nodiscard]] std::string describe_mechanisms(MechanismSet set);
[[nodiscard]] Mechanism primary_mechanism(MechanismSet set) noexcept;

enum class Severity : std::uint8_t {
  kNone = 0,
  kInformational = 1,
  kLow = 2,
  kModerate = 3,
  kHigh = 4,
  kCritical = 5,
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

// A blocker is a named reason why the assessment is weaker than a naive reading would suggest.
struct Blocker {
  std::string code{};
  std::string detail{};

  friend bool operator==(const Blocker& lhs, const Blocker& rhs) noexcept {
    return lhs.code == rhs.code && lhs.detail == rhs.detail;
  }
};

struct ExplanationStep {
  std::string rule_id{};
  std::string decision{};
  std::string reason_code{};
  std::vector<EvidenceId> citations{};
};

struct Explanation {
  std::vector<ExplanationStep> steps{};
  std::vector<Blocker> blockers{};
  std::string policy_version{};
  std::uint64_t policy_digest{0};
  std::string policy_digest_hex{};

  [[nodiscard]] bool has_blocker(std::string_view code) const noexcept;
  [[nodiscard]] std::string render() const;
};

// How the confidence number was reached. Exposed so that a low confidence is explainable rather
// than mysterious.
struct ConfidenceBasis {
  std::size_t deciding_records{0};
  std::size_t corroborating_sources{0};
  std::size_t fresh_records{0};
  std::size_t aging_records{0};
  std::size_t stale_records{0};
  std::size_t expired_records{0};
  std::size_t unknown_freshness_records{0};
  std::size_t conflicting_features{0};
  std::size_t unsupported_records{0};
  std::size_t incomplete_records{0};
  std::size_t raw_points{0};
};

// Feature extraction result. Every verdict is a pure function of this snapshot plus the policy,
// which is what makes the decision reproducible and auditable.
struct FeatureSnapshot {
  EvidenceSubject subject{};
  std::size_t records_considered{0};
  std::size_t records_in_window{0};
  std::size_t records_fresh{0};
  std::size_t records_aging{0};
  std::size_t records_stale{0};
  std::size_t records_expired{0};
  std::size_t records_unknown_freshness{0};
  std::size_t records_generation_mismatch{0};
  std::size_t records_recovered{0};
  std::size_t records_retired{0};
  std::size_t deciding_sources{0};
  // Ascending, de-duplicated list of sources that produced deciding evidence.
  std::vector<SourceId> deciding_source_list{};
  bool any_fresh_deciding{false};

  bool utilization_present{false};
  bool utilization_seen_not_fresh{false};
  double utilization{0.0};
  Freshness utilization_freshness{Freshness::kUnknown};
  Agreement utilization_agreement{Agreement::kUnknown};
  bool utilization_exceeds_attention{false};
  bool utilization_exceeds_high{false};

  bool pressure_present{false};
  bool pressure_seen_not_fresh{false};
  bool pressure_ratio_available{false};
  bool pressure_exceeds_ratio{false};
  double pressure_ratio{0.0};
  Freshness pressure_freshness{Freshness::kUnknown};
  Agreement pressure_agreement{Agreement::kUnknown};

  bool contention_present{false};
  bool contention_seen_not_fresh{false};
  double contention_degree{0.0};
  Freshness contention_freshness{Freshness::kUnknown};

  bool demand_present{false};
  bool demand_seen_not_fresh{false};
  bool capacity_known{false};
  double offered_bps{0.0};
  double achieved_bps{0.0};
  double capacity_bps{0.0};
  double demand_capacity_ratio{0.0};
  bool demand_exceeds_capacity{false};
  Freshness demand_freshness{Freshness::kUnknown};
  Agreement demand_agreement{Agreement::kUnknown};

  bool impairment_present{false};
  bool impairment_seen_not_fresh{false};
  bool impairment_unquantified{false};
  bool impairment_counter_without_delta{false};
  bool impairment_below_threshold{false};
  bool latency_inflation_demonstrated{false};
  double drop_rate{0.0};
  double mark_rate{0.0};
  double pause_rate{0.0};
  double retransmit_rate{0.0};
  double latency_factor{0.0};
  Freshness impairment_freshness{Freshness::kUnknown};
  Agreement impairment_agreement{Agreement::kUnknown};
  MechanismSet observed_mechanisms{0};

  bool unsupported_evidence_seen{false};
  bool incomplete_evidence_seen{false};
  std::size_t unsupported_records{0};
  std::size_t incomplete_records{0};

  std::uint64_t feature_digest{0};
};

struct ClassificationRequest {
  EvidenceSubject subject{};
  Timestamp window_start{};
  Timestamp window_end{};
  Timestamp evaluated_at{};
  GenerationVector generation{};
  // Optional tenant attribution. An invalid id means "not attributed", which is different from
  // "attributed to the empty tenant".
  TenantId tenant{};
  std::size_t max_citations{64};
};

struct CongestionAssessment {
  EvidenceSubject subject{};
  Verdict verdict{Verdict::kNoEvidence};
  MechanismSet mechanisms{0};
  Severity severity{Severity::kNone};
  std::uint32_t confidence{0};  // 0..100
  ConfidenceBasis basis{};
  FeatureSnapshot features{};
  std::vector<EvidenceId> citations{};
  std::vector<Blocker> blockers{};
  Explanation explanation{};
  Timestamp window_start{};
  Timestamp window_end{};
  Timestamp evaluated_at{};
  GenerationVector generation{};
  TenantId tenant{};
  Agreement deciding_agreement{Agreement::kUnknown};
  // Sources that contributed deciding evidence, ascending and de-duplicated.
  std::vector<SourceId> sources{};
  bool congestion_asserted{false};

  [[nodiscard]] bool has_blocker(std::string_view code) const noexcept;
};

// Pure classification over an explicit evidence window. The caller supplies the records (already
// filtered to the subject by the store) so this function is trivially property-testable.
[[nodiscard]] Result<CongestionAssessment> classify_subject(
    const ClassificationRequest& request, const std::vector<const EvidenceRecord*>& records,
    const ClassificationPolicy& policy);

// Extracts the feature snapshot without deciding a verdict. Exposed for inspection tooling and
// for tests that assert on feature level distinctions.
[[nodiscard]] FeatureSnapshot extract_features(const ClassificationRequest& request,
                                               const std::vector<const EvidenceRecord*>& records,
                                               const ClassificationPolicy& policy);

// Well known blocker codes. Tests and explanations reference these exact strings.
namespace blockers {
inline constexpr std::string_view kUtilizationAlone = "utilization_alone_is_not_congestion";
inline constexpr std::string_view kStalePressureCannotProveCurrent = "stale_pressure_cannot_prove_current_congestion";
inline constexpr std::string_view kStaleImpairment = "stale_impairment_cannot_prove_current_congestion";
inline constexpr std::string_view kConflictingSources = "conflicting_sources_visible";
inline constexpr std::string_view kSingleSourceUncorroborated = "single_source_uncorroborated";
inline constexpr std::string_view kUnsupportedEvidence = "evidence_unsupported_for_subject";
inline constexpr std::string_view kIncompleteEvidence = "evidence_incomplete";
inline constexpr std::string_view kNoDemandContext = "no_demand_context_available";
inline constexpr std::string_view kUnknownFreshness = "freshness_unknown";
inline constexpr std::string_view kAbsenceOfEvidence = "absence_of_evidence_is_not_evidence";
inline constexpr std::string_view kGenerationMismatch = "evidence_generation_mismatch";
inline constexpr std::string_view kPersistedNotLive = "persisted_evidence_is_not_live";
inline constexpr std::string_view kRecoveredHistoryOnly = "recovered_history_only";
inline constexpr std::string_view kNoFreshEvidence = "no_fresh_deciding_evidence";
inline constexpr std::string_view kImpairmentBelowThreshold = "impairment_below_policy_threshold";
inline constexpr std::string_view kCounterWithoutDelta = "cumulative_counter_cannot_prove_current_harm";
inline constexpr std::string_view kRetiredIncarnation = "evidence_from_retired_source_incarnation";
inline constexpr std::string_view kLatencyBaselineMissing = "latency_baseline_missing";
inline constexpr std::string_view kUtilizationNotCongestion = "utilization_alone_is_not_congestion";
inline constexpr std::string_view kBacklogBelowThreshold = "backlog_below_policy_threshold";
}  // namespace blockers

}  // namespace congestion

#endif  // CONGESTION_ASSESSMENT_CLASSIFY_HPP
