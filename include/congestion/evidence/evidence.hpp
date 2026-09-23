// Congestion Observatory - the observation record and its epistemic state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_EVIDENCE_EVIDENCE_HPP
#define CONGESTION_EVIDENCE_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/core/time.hpp"
#include "congestion/model/generation.hpp"
#include "congestion/model/identities.hpp"

namespace congestion {

// What a record physically reports. The kind determines the role the record can play in a
// congestion argument; see role_of().
enum class EvidenceKind : std::uint16_t {
  kUnknown = 0,
  kLinkUtilization = 1,
  kPortUtilization = 2,
  kQueueOccupancy = 3,
  kQueueDepth = 4,
  kBufferOccupancy = 5,
  kDropCount = 6,
  kMarkCount = 7,
  kPauseCount = 8,
  kPauseDuration = 9,
  kLatencySample = 10,
  kOfferedDemand = 11,
  kAchievedThroughput = 12,
  kActiveFlowCount = 13,
  kActiveClassCount = 14,
  kOperState = 15,
  kSourceHeartbeat = 16,
  kTopologyAdvertisement = 17,
  kCapacityAdvertisement = 18,
  kRetransmitCount = 19,
};

inline constexpr std::size_t kEvidenceKindCount = 20;

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] Result<EvidenceKind> evidence_kind_from_string(std::string_view text);

// The role is the *argumentative* category of evidence, and it is the heart of the boundary this
// runtime implements:
//   kUtilization - how much of a capacity is consumed. High utilization alone is never proof.
//   kContention  - multiple independent consumers compete for one resource.
//   kPressure    - a backlog exists (queue/buffer occupancy above baseline).
//   kImpairment  - traffic was actually harmed (drop, mark, pause, latency inflation, retransmit).
//   kDemand      - offered or achieved load, needed to interpret every other role.
//   kCapacity    - the advertised capacity that utilization is relative to.
//   kTopology    - structural advertisement.
//   kLiveness    - the source itself is alive.
enum class EvidenceRole : std::uint8_t {
  kUnknown = 0,
  kUtilization = 1,
  kContention = 2,
  kPressure = 3,
  kImpairment = 4,
  kDemand = 5,
  kCapacity = 6,
  kTopology = 7,
  kLiveness = 8,
};

[[nodiscard]] EvidenceRole role_of(EvidenceKind kind) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceRole role) noexcept;
[[nodiscard]] bool is_impairment_role(EvidenceKind kind) noexcept;

enum class ObservationUnit : std::uint8_t {
  kUnknown = 0,
  kRatio = 1,
  kCells = 2,
  kPackets = 3,
  kBytes = 4,
  kNanoseconds = 5,
  kBitsPerSecond = 6,
  kCount = 7,
  kBoolean = 8,
  kFrames = 9,
};

[[nodiscard]] std::string_view to_string(ObservationUnit unit) noexcept;
[[nodiscard]] Result<ObservationUnit> observation_unit_from_string(std::string_view text);

enum class ValueSemantics : std::uint8_t {
  kUnknown = 0,
  kGauge = 1,              // instantaneous level
  kCumulativeCounter = 2,  // monotonically increasing since source boot
  kDeltaCounter = 3,       // increment over the record's own interval
  kRate = 4,               // already normalised per second
};

[[nodiscard]] std::string_view to_string(ValueSemantics semantics) noexcept;
[[nodiscard]] Result<ValueSemantics> value_semantics_from_string(std::string_view text);

// Whether the observation *could* be made for this object/kind at all. A source that cannot
// observe a quantity reports kUnsupported, which is materially different from reporting zero.
enum class Support : std::uint8_t {
  kSupported = 0,
  kUnsupported = 1,
  kUnknown = 2,
};

[[nodiscard]] std::string_view to_string(Support support) noexcept;
[[nodiscard]] Result<Support> support_from_string(std::string_view text);

// Whether all the inputs the source needed were available.
enum class Completeness : std::uint8_t {
  kComplete = 0,
  kPartial = 1,
  kIncomplete = 2,
  kUnknown = 3,
};

[[nodiscard]] std::string_view to_string(Completeness completeness) noexcept;
[[nodiscard]] Result<Completeness> completeness_from_string(std::string_view text);

// Freshness of a record relative to an explicit evaluation instant.
enum class Freshness : std::uint8_t {
  kFresh = 0,    // usable as deciding evidence
  kAging = 1,    // usable as corroboration only
  kStale = 2,    // cannot prove anything about the present
  kExpired = 3,  // beyond its validity window
  kUnknown = 4,  // timestamps or clock domain unusable
};

[[nodiscard]] std::string_view to_string(Freshness freshness) noexcept;
[[nodiscard]] bool usable_as_deciding_evidence(Freshness freshness) noexcept;
[[nodiscard]] bool usable_as_corroboration(Freshness freshness) noexcept;

// Agreement between independent sources describing the same subject/kind/window.
enum class Agreement : std::uint8_t {
  kUnknown = 0,       // no records
  kSingleSource = 1,  // nothing corroborates or contradicts: not "conflict free"
  kUnanimous = 2,
  kMinority = 3,      // a dissenting minority exists; both positions stay visible
  kConflicting = 4,   // no position holds a majority
  kIncomparable = 5,  // units/semantics differ, so the values cannot be compared at all
};

[[nodiscard]] std::string_view to_string(Agreement agreement) noexcept;

struct EvidenceValue {
  double scalar{0.0};
  ObservationUnit unit{ObservationUnit::kUnknown};
  ValueSemantics semantics{ValueSemantics::kUnknown};
};

// Provenance answers "who said this, how, and under which incarnation".
struct Provenance {
  SourceId source{};
  AuthorityLevel authority{AuthorityLevel::kUnknown};
  std::string transport{};   // transport/agent description, bounded
  std::string collector{};   // collector instance description, bounded
};

struct EvidenceRecord {
  EvidenceId id{};
  EvidenceKind kind{EvidenceKind::kUnknown};
  EvidenceSubject subject{};
  Provenance provenance{};
  FenceVector fence{};
  Timestamp observed_at{};
  Timestamp received_at{};
  ClockDomain clock{ClockDomain::kUnknown};
  EvidenceValue value{};
  Duration validity{Duration::from_seconds(5)};
  Completeness completeness{Completeness::kComplete};
  Support support{Support::kSupported};
  std::vector<Name> labels{};
  std::vector<std::pair<std::string, std::string>> metadata{};
  std::string note{};
  // True when the record was reconstructed from a snapshot rather than received live. Such a
  // record is history: it is never treated as live proof, regardless of its timestamps.
  bool recovered_from_snapshot{false};
  // True when a newer incarnation of the same source superseded this observation. The record is
  // retained as history but can never contribute to a current verdict.
  bool retired{false};
  // Monotonic insertion index assigned by the store; not part of the identity.
  std::uint64_t insertion_index{0};
};

// Evidence ids are derived from the canonical encoding of the identity-bearing fields. The same
// observation therefore has the same id in every process, run and build.
[[nodiscard]] EvidenceId compute_evidence_id(const EvidenceRecord& record) noexcept;

// Canonical encoding used for identity derivation and digest computation.
void encode_evidence_identity(const EvidenceRecord& record, StableHasher& hasher) noexcept;

// Structural validation of a record against the configured bounds.
[[nodiscard]] Status validate_evidence(const EvidenceRecord& record, const Limits& limits);

// Freshness policy. Thresholds are explicit and versioned; nothing is inferred from defaults
// hidden in code.
struct FreshnessPolicy {
  Duration fresh_within{Duration::from_seconds(2)};
  Duration aging_within{Duration::from_seconds(10)};
  Duration stale_within{Duration::from_seconds(60)};
  Duration max_future_skew{Duration::from_seconds(1)};
  bool require_plausible_timestamps{true};
};

struct FreshnessAssessment {
  Freshness freshness{Freshness::kUnknown};
  Duration age{};
  std::string reason_code{};

  [[nodiscard]] bool usable_as_decision() const noexcept {
    return usable_as_deciding_evidence(freshness);
  }
};

// Evaluates freshness of a record at an explicit instant. Records recovered from a snapshot are
// reported as kUnknown with reason "persisted_dynamic_evidence_not_live": restart preserves
// history, never liveness.
[[nodiscard]] FreshnessAssessment assess_freshness(const EvidenceRecord& record, Timestamp now,
                                                  const FreshnessPolicy& policy) noexcept;

struct AgreementAssessment {
  Agreement agreement{Agreement::kUnknown};
  std::size_t records_considered{0};
  std::size_t distinct_sources{0};
  double representative_value{0.0};
  double min_value{0.0};
  double max_value{0.0};
  double relative_spread{0.0};
  std::vector<EvidenceId> citations{};  // ascending, both sides of a disagreement
  std::string reason_code{};
};

// Compares records that describe the same subject and kind inside one evaluation window. Values
// are clustered with a relative tolerance; the result never hides a dissenting source.
[[nodiscard]] AgreementAssessment assess_agreement(const std::vector<const EvidenceRecord*>& records,
                                                   double relative_tolerance,
                                                   std::size_t max_citations);

}  // namespace congestion

#endif  // CONGESTION_EVIDENCE_EVIDENCE_HPP
