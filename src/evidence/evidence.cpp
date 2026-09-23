// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/evidence/evidence.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace congestion {
namespace {

struct KindInfo {
  EvidenceKind kind;
  const char* name;
  EvidenceRole role;
};

// The single source of truth mapping a measurement to the argumentative role it may play.
constexpr KindInfo kKindTable[] = {
    {EvidenceKind::kUnknown, "unknown", EvidenceRole::kUnknown},
    {EvidenceKind::kLinkUtilization, "link_utilization", EvidenceRole::kUtilization},
    {EvidenceKind::kPortUtilization, "port_utilization", EvidenceRole::kUtilization},
    {EvidenceKind::kQueueOccupancy, "queue_occupancy", EvidenceRole::kPressure},
    {EvidenceKind::kQueueDepth, "queue_depth", EvidenceRole::kPressure},
    {EvidenceKind::kBufferOccupancy, "buffer_occupancy", EvidenceRole::kPressure},
    {EvidenceKind::kDropCount, "drop_count", EvidenceRole::kImpairment},
    {EvidenceKind::kMarkCount, "mark_count", EvidenceRole::kImpairment},
    {EvidenceKind::kPauseCount, "pause_count", EvidenceRole::kImpairment},
    {EvidenceKind::kPauseDuration, "pause_duration", EvidenceRole::kImpairment},
    {EvidenceKind::kLatencySample, "latency_sample", EvidenceRole::kImpairment},
    {EvidenceKind::kOfferedDemand, "offered_demand", EvidenceRole::kDemand},
    {EvidenceKind::kAchievedThroughput, "achieved_throughput", EvidenceRole::kDemand},
    {EvidenceKind::kActiveFlowCount, "active_flow_count", EvidenceRole::kContention},
    {EvidenceKind::kActiveClassCount, "active_class_count", EvidenceRole::kContention},
    {EvidenceKind::kOperState, "oper_state", EvidenceRole::kTopology},
    {EvidenceKind::kSourceHeartbeat, "source_heartbeat", EvidenceRole::kLiveness},
    {EvidenceKind::kTopologyAdvertisement, "topology_advertisement", EvidenceRole::kTopology},
    {EvidenceKind::kCapacityAdvertisement, "capacity_advertisement", EvidenceRole::kCapacity},
    {EvidenceKind::kRetransmitCount, "retransmit_count", EvidenceRole::kImpairment},
};

constexpr std::size_t kKindTableSize = sizeof(kKindTable) / sizeof(kKindTable[0]);

struct UnitInfo {
  ObservationUnit value;
  const char* name;
};

constexpr UnitInfo kUnitTable[] = {
    {ObservationUnit::kUnknown, "unknown"},   {ObservationUnit::kRatio, "ratio"},
    {ObservationUnit::kCells, "cells"},       {ObservationUnit::kPackets, "packets"},
    {ObservationUnit::kBytes, "bytes"},       {ObservationUnit::kNanoseconds, "nanoseconds"},
    {ObservationUnit::kBitsPerSecond, "bps"}, {ObservationUnit::kCount, "count"},
    {ObservationUnit::kBoolean, "boolean"},   {ObservationUnit::kFrames, "frames"},
};

struct SemanticsInfo {
  ValueSemantics value;
  const char* name;
};

constexpr SemanticsInfo kSemanticsTable[] = {
    {ValueSemantics::kUnknown, "unknown"},
    {ValueSemantics::kGauge, "gauge"},
    {ValueSemantics::kCumulativeCounter, "cumulative_counter"},
    {ValueSemantics::kDeltaCounter, "delta_counter"},
    {ValueSemantics::kRate, "rate"},
};

const char* lookup(EvidenceKind kind) {
  for (const KindInfo& info : kKindTable) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "unknown";
}

template <class Table, class Enum>
const char* lookup_enum(const Table& table, std::size_t size, Enum value, const char* fallback) {
  for (std::size_t i = 0; i < size; ++i) {
    if (table[i].value == value) {
      return table[i].name;
    }
  }
  return fallback;
}

}  // namespace

std::string_view to_string(EvidenceKind kind) noexcept { return lookup(kind); }

Result<EvidenceKind> evidence_kind_from_string(std::string_view text) {
  for (const KindInfo& info : kKindTable) {
    if (text == info.name) {
      return info.kind;
    }
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown evidence kind", std::string(text));
}

EvidenceRole role_of(EvidenceKind kind) noexcept {
  for (const KindInfo& info : kKindTable) {
    if (info.kind == kind) {
      return info.role;
    }
  }
  return EvidenceRole::kUnknown;
}

bool is_impairment_role(EvidenceKind kind) noexcept {
  return role_of(kind) == EvidenceRole::kImpairment;
}

std::string_view to_string(EvidenceRole role) noexcept {
  switch (role) {
    case EvidenceRole::kUnknown: return "unknown";
    case EvidenceRole::kUtilization: return "utilization";
    case EvidenceRole::kContention: return "contention";
    case EvidenceRole::kPressure: return "pressure";
    case EvidenceRole::kImpairment: return "impairment";
    case EvidenceRole::kDemand: return "demand";
    case EvidenceRole::kCapacity: return "capacity";
    case EvidenceRole::kTopology: return "topology";
    case EvidenceRole::kLiveness: return "liveness";
  }
  return "unknown";
}

std::string_view to_string(ObservationUnit unit) noexcept {
  return lookup_enum(kUnitTable, sizeof(kUnitTable) / sizeof(kUnitTable[0]), unit, "unknown");
}

Result<ObservationUnit> observation_unit_from_string(std::string_view text) {
  for (const UnitInfo& info : kUnitTable) {
    if (text == info.name) {
      return info.value;
    }
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown observation unit", std::string(text));
}

std::string_view to_string(ValueSemantics semantics) noexcept {
  return lookup_enum(kSemanticsTable, sizeof(kSemanticsTable) / sizeof(kSemanticsTable[0]),
                     semantics, "unknown");
}

Result<ValueSemantics> value_semantics_from_string(std::string_view text) {
  for (const SemanticsInfo& info : kSemanticsTable) {
    if (text == info.name) {
      return info.value;
    }
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown value semantics", std::string(text));
}

std::string_view to_string(Support support) noexcept {
  switch (support) {
    case Support::kSupported: return "supported";
    case Support::kUnsupported: return "unsupported";
    case Support::kUnknown: return "unknown";
  }
  return "unknown";
}

Result<Support> support_from_string(std::string_view text) {
  if (text == "supported") return Support::kSupported;
  if (text == "unsupported") return Support::kUnsupported;
  if (text == "unknown") return Support::kUnknown;
  return make_error(ErrorCode::kInvalidArgument, "unknown support value", std::string(text));
}

std::string_view to_string(Completeness completeness) noexcept {
  switch (completeness) {
    case Completeness::kComplete: return "complete";
    case Completeness::kPartial: return "partial";
    case Completeness::kIncomplete: return "incomplete";
    case Completeness::kUnknown: return "unknown";
  }
  return "unknown";
}

Result<Completeness> completeness_from_string(std::string_view text) {
  if (text == "complete") return Completeness::kComplete;
  if (text == "partial") return Completeness::kPartial;
  if (text == "incomplete") return Completeness::kIncomplete;
  if (text == "unknown") return Completeness::kUnknown;
  return make_error(ErrorCode::kInvalidArgument, "unknown completeness value", std::string(text));
}

std::string_view to_string(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::kFresh: return "fresh";
    case Freshness::kAging: return "aging";
    case Freshness::kStale: return "stale";
    case Freshness::kExpired: return "expired";
    case Freshness::kUnknown: return "unknown";
  }
  return "unknown";
}

bool usable_as_deciding_evidence(Freshness freshness) noexcept {
  return freshness == Freshness::kFresh;
}

bool usable_as_corroboration(Freshness freshness) noexcept {
  return freshness == Freshness::kFresh || freshness == Freshness::kAging;
}

std::string_view to_string(Agreement agreement) noexcept {
  switch (agreement) {
    case Agreement::kUnknown: return "unknown";
    case Agreement::kSingleSource: return "single_source";
    case Agreement::kUnanimous: return "unanimous";
    case Agreement::kMinority: return "minority";
    case Agreement::kConflicting: return "conflicting";
    case Agreement::kIncomparable: return "incomparable";
  }
  return "unknown";
}

void encode_evidence_identity(const EvidenceRecord& record, StableHasher& hasher) noexcept {
  hasher.update(record.provenance.source.view());
  hasher.separator();
  hasher.update_u64(record.fence.boot.value());
  hasher.update_u64(record.fence.incarnation.value());
  hasher.update_u64(record.fence.gen.epoch.value());
  hasher.update_u64(record.fence.gen.generation.value());
  hasher.update_u64(record.fence.gen.revision.value());
  hasher.update_u64(record.fence.sequence.value());
  hasher.update_u64(static_cast<std::uint64_t>(record.kind));
  hasher.update_u64(static_cast<std::uint64_t>(record.subject.kind()));
  hasher.update(record.subject.str());
  hasher.update_i64(record.observed_at.unix_nanos());
  hasher.update_u64(static_cast<std::uint64_t>(record.value.unit));
  hasher.update_u64(static_cast<std::uint64_t>(record.value.semantics));
}

EvidenceId compute_evidence_id(const EvidenceRecord& record) noexcept {
  StableHasher hasher;
  encode_evidence_identity(record, hasher);
  return EvidenceId::from_digest(hasher.digest128());
}

Status validate_evidence(const EvidenceRecord& record, const Limits& limits) {
  if (record.kind == EvidenceKind::kUnknown) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence kind is unknown"));
  }
  if (!record.subject.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence subject is empty"));
  }
  if (!record.provenance.source.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence source is missing"));
  }
  if (record.fence.source != record.provenance.source) {
    return Status(make_error(ErrorCode::kPreconditionFailed,
                             "fence source does not match the provenance source",
                             record.fence.source.str() + " != " + record.provenance.source.str()));
  }
  if (record.support == Support::kSupported && record.value.unit == ObservationUnit::kUnknown) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "supported evidence requires an observation unit",
                             record.subject.str()));
  }
  if (!std::isfinite(record.value.scalar)) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence value is not finite",
                             record.subject.str()));
  }
  if (record.validity.is_negative()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence validity is negative"));
  }
  if (record.labels.size() > limits.max_labels) {
    return Status(make_error(ErrorCode::kLimitExceeded, "evidence label count exceeds the limit"));
  }
  for (const Name& label : record.labels) {
    if (!label.valid()) {
      return Status(make_error(ErrorCode::kInvalidArgument, "evidence label is invalid"));
    }
  }
  if (record.metadata.size() > limits.max_metadata_entries) {
    return Status(make_error(ErrorCode::kLimitExceeded, "evidence metadata count exceeds the limit"));
  }
  for (const auto& entry : record.metadata) {
    if (entry.first.size() > limits.max_metadata_key_bytes) {
      return Status(make_error(ErrorCode::kLimitExceeded, "evidence metadata key exceeds the limit"));
    }
    if (entry.second.size() > limits.max_metadata_value_bytes) {
      return Status(make_error(ErrorCode::kLimitExceeded,
                               "evidence metadata value exceeds the limit"));
    }
  }
  if (record.note.size() > limits.max_note_bytes) {
    return Status(make_error(ErrorCode::kLimitExceeded, "evidence note exceeds the limit"));
  }
  return Status{};
}

FreshnessAssessment assess_freshness(const EvidenceRecord& record, Timestamp now,
                                     const FreshnessPolicy& policy) noexcept {
  FreshnessAssessment result;
  if (record.recovered_from_snapshot) {
    // Restart preserves history, never liveness: a record that was fresh before the restart is
    // not evidence about the present.
    result.freshness = Freshness::kUnknown;
    result.reason_code = "persisted_dynamic_evidence_not_live";
    return result;
  }
  if (record.retired) {
    // The source reincarnated after this observation: the record describes a world that no
    // longer exists.
    result.freshness = Freshness::kUnknown;
    result.reason_code = "superseded_by_newer_source_incarnation";
    return result;
  }
  if (record.clock == ClockDomain::kUnknown) {
    result.freshness = Freshness::kUnknown;
    result.reason_code = "clock_domain_unknown";
    return result;
  }
  if (record.observed_at.is_zero() || record.received_at.is_zero()) {
    result.freshness = Freshness::kUnknown;
    result.reason_code = "timestamp_missing";
    return result;
  }
  if (policy.require_plausible_timestamps) {
    if (record.observed_at > record.received_at + policy.max_future_skew) {
      result.freshness = Freshness::kUnknown;
      result.reason_code = "observation_after_receive_by_more_than_skew";
      return result;
    }
    if (record.received_at > now + policy.max_future_skew) {
      result.freshness = Freshness::kUnknown;
      result.reason_code = "receive_time_in_future";
      return result;
    }
    if (record.observed_at > now + policy.max_future_skew) {
      result.freshness = Freshness::kUnknown;
      result.reason_code = "observation_in_future";
      return result;
    }
  }
  const Duration age = now - record.observed_at;
  result.age = age;
  if (age > policy.stale_within || age > record.validity) {
    result.freshness = Freshness::kExpired;
    result.reason_code = "beyond_validity_window";
    return result;
  }
  if (age > policy.aging_within) {
    result.freshness = Freshness::kStale;
    result.reason_code = "older_than_aging_window";
    return result;
  }
  if (age > policy.fresh_within) {
    result.freshness = Freshness::kAging;
    result.reason_code = "older_than_fresh_window";
    return result;
  }
  result.freshness = Freshness::kFresh;
  result.reason_code = "within_fresh_window";
  return result;
}

AgreementAssessment assess_agreement(const std::vector<const EvidenceRecord*>& records,
                                     double relative_tolerance, std::size_t max_citations) {
  AgreementAssessment result;
  result.records_considered = records.size();
  if (records.empty()) {
    result.agreement = Agreement::kUnknown;
    result.reason_code = "no_records";
    return result;
  }

  ObservationUnit unit = records.front()->value.unit;
  ValueSemantics semantics = records.front()->value.semantics;
  bool comparable = true;
  for (const EvidenceRecord* record : records) {
    if (record->value.unit != unit) {
      comparable = false;
    }
  }
  if (semantics == ValueSemantics::kCumulativeCounter && records.size() > 1) {
    // Cumulative counters from different sources have different epochs: they cannot be compared.
    comparable = false;
  }

  std::vector<const EvidenceRecord*> ordered = records;
  std::sort(ordered.begin(), ordered.end(),
            [](const EvidenceRecord* lhs, const EvidenceRecord* rhs) {
              if (lhs->value.scalar != rhs->value.scalar) {
                return lhs->value.scalar < rhs->value.scalar;
              }
              if (lhs->provenance.source != rhs->provenance.source) {
                return lhs->provenance.source < rhs->provenance.source;
              }
              return lhs->id < rhs->id;
            });

  std::map<Name, const EvidenceRecord*> distinct;
  for (const EvidenceRecord* record : ordered) {
    distinct.emplace(record->provenance.source.name(), record);
  }
  result.distinct_sources = distinct.size();
  result.min_value = ordered.front()->value.scalar;
  result.max_value = ordered.back()->value.scalar;
  const double denominator = std::max(std::abs(result.min_value), 1e-12);
  result.relative_spread = (result.max_value - result.min_value) / denominator;

  for (const EvidenceRecord* record : ordered) {
    result.citations.push_back(record->id);
  }
  if (result.citations.size() > max_citations) {
    result.citations.resize(max_citations);
    result.reason_code = "citations_truncated";
  }
  std::sort(result.citations.begin(), result.citations.end());

  if (!comparable) {
    result.agreement = Agreement::kIncomparable;
    if (result.reason_code.empty()) {
      result.reason_code = "units_or_semantics_not_comparable";
    }
    return result;
  }
  if (result.distinct_sources <= 1) {
    result.agreement = Agreement::kSingleSource;
    result.representative_value = ordered.front()->value.scalar;
    if (result.reason_code.empty()) {
      result.reason_code = "single_source_no_corroboration";
    }
    return result;
  }

  // Greedy clustering in ascending value order: two values belong to the same cluster when they
  // are within the relative tolerance of the first member of the cluster.
  std::vector<std::vector<const EvidenceRecord*>> clusters;
  std::map<Name, std::size_t> first_cluster_of_source;
  for (const EvidenceRecord* record : ordered) {
    bool placed = false;
    for (std::size_t index = 0; index < clusters.size(); ++index) {
      const double reference = clusters[index].front()->value.scalar;
      const double scale = std::max(std::abs(reference), 1e-12);
      if (std::abs(record->value.scalar - reference) <= relative_tolerance * scale) {
        clusters[index].push_back(record);
        first_cluster_of_source.emplace(record->provenance.source.name(), index);
        placed = true;
        break;
      }
    }
    if (!placed) {
      clusters.push_back({record});
      first_cluster_of_source.emplace(record->provenance.source.name(), clusters.size() - 1);
    }
  }

  // Count distinct sources per cluster. A source that reported several values is counted once,
  // against the first cluster it appears in, so a chatty source cannot manufacture a majority.
  std::vector<std::size_t> sources_per_cluster(clusters.size(), 0);
  for (const auto& entry : first_cluster_of_source) {
    if (entry.second < sources_per_cluster.size()) {
      sources_per_cluster[entry.second] += 1;
    }
  }

  std::size_t majority_index = 0;
  for (std::size_t i = 1; i < sources_per_cluster.size(); ++i) {
    if (sources_per_cluster[i] > sources_per_cluster[majority_index]) {
      majority_index = i;
    }
  }
  const std::size_t majority_sources = sources_per_cluster[majority_index];
  double total = 0.0;
  for (const EvidenceRecord* record : clusters[majority_index]) {
    total += record->value.scalar;
  }
  result.representative_value =
      clusters[majority_index].empty()
          ? 0.0
          : total / static_cast<double>(clusters[majority_index].size());

  if (clusters.size() == 1) {
    result.agreement = Agreement::kUnanimous;
    if (result.reason_code.empty()) {
      result.reason_code = "all_sources_agree";
    }
    return result;
  }
  if (majority_sources * 2 > result.distinct_sources) {
    result.agreement = Agreement::kMinority;
    if (result.reason_code.empty()) {
      result.reason_code = "dissenting_minority_present";
    }
    return result;
  }
  result.agreement = Agreement::kConflicting;
  if (result.reason_code.empty()) {
    result.reason_code = "no_majority_position";
  }
  return result;
}

}  // namespace congestion
