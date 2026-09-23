// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/assessment/classify.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace congestion {
namespace {

// Freshness ordering used to report the best available freshness per role.
int freshness_rank(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::kFresh: return 0;
    case Freshness::kAging: return 1;
    case Freshness::kStale: return 2;
    case Freshness::kExpired: return 3;
    case Freshness::kUnknown: return 4;
  }
  return 4;
}

Freshness best_freshness(Freshness lhs, Freshness rhs) noexcept {
  return freshness_rank(lhs) <= freshness_rank(rhs) ? lhs : rhs;
}

bool generation_matches(const EvidenceRecord& record, const GenerationVector& requested) noexcept {
  const bool unconstrained = requested.epoch.is_zero() && requested.generation.is_zero() &&
                             requested.revision.is_zero();
  return unconstrained || record.fence.gen == requested;
}

bool agreement_is_conflict(Agreement agreement) noexcept {
  return agreement == Agreement::kConflicting || agreement == Agreement::kMinority;
}

double max_scalar(const std::vector<const EvidenceRecord*>& records) {
  double value = 0.0;
  bool first = true;
  for (const EvidenceRecord* record : records) {
    if (first || record->value.scalar > value) {
      value = record->value.scalar;
      first = false;
    }
  }
  return first ? 0.0 : value;
}

bool any_ratio_unit(const std::vector<const EvidenceRecord*>& records) {
  for (const EvidenceRecord* record : records) {
    if (record->value.unit == ObservationUnit::kRatio) {
      return true;
    }
  }
  return false;
}

double max_ratio(const std::vector<const EvidenceRecord*>& records) {
  double value = 0.0;
  for (const EvidenceRecord* record : records) {
    if (record->value.unit == ObservationUnit::kRatio && record->value.scalar > value) {
      value = record->value.scalar;
    }
  }
  return value;
}

bool is_counting_unit(ObservationUnit unit) noexcept {
  return unit == ObservationUnit::kPackets || unit == ObservationUnit::kFrames ||
         unit == ObservationUnit::kCount || unit == ObservationUnit::kCells;
}

struct RateObservation {
  double rate{0.0};             // ratio-unit observation
  bool unquantified{false};     // positive delta counter of a counting unit
  bool counter_without_delta{false};  // cumulative counter: cannot prove present harm
  bool observed{false};
};

RateObservation observe_rate(const std::vector<const EvidenceRecord*>& records,
                             EvidenceKind kind) {
  RateObservation observation;
  for (const EvidenceRecord* record : records) {
    if (record->kind != kind) {
      continue;
    }
    if (record->value.unit == ObservationUnit::kRatio) {
      if (record->value.scalar > observation.rate) {
        observation.rate = record->value.scalar;
      }
      observation.observed = observation.observed || record->value.scalar > 0.0;
      continue;
    }
    if (!is_counting_unit(record->value.unit)) {
      if (record->value.scalar > 0.0) {
        observation.observed = true;
        observation.unquantified = true;
      }
      continue;
    }
    if (record->value.semantics == ValueSemantics::kCumulativeCounter) {
      observation.counter_without_delta = true;
      continue;
    }
    if (record->value.scalar > 0.0) {
      observation.observed = true;
      observation.unquantified = true;
    }
  }
  return observation;
}

void hash_feature_value(StableHasher& hasher, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  hasher.update_u64(bits);
}

std::uint32_t clamp_confidence(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return static_cast<std::uint32_t>(value);
}

std::size_t popcount(MechanismSet set) noexcept {
  return static_cast<std::size_t>(std::popcount(set));
}

Severity severity_for_rates(const FeatureSnapshot& features, const ClassificationPolicy& policy) {
  const double drop_ratio =
      policy.drop_rate_threshold > 0.0 ? features.drop_rate / policy.drop_rate_threshold : 0.0;
  const double mark_ratio =
      policy.mark_rate_threshold > 0.0 ? features.mark_rate / policy.mark_rate_threshold : 0.0;
  const double worst = std::max(drop_ratio, mark_ratio);
  if (worst >= 100.0 || popcount(features.observed_mechanisms) >= 3) {
    return Severity::kCritical;
  }
  if (worst >= 10.0 || popcount(features.observed_mechanisms) >= 2) {
    return Severity::kHigh;
  }
  return Severity::kModerate;
}

class RuleEngine {
 public:
  RuleEngine(const ClassificationRequest& request, const ClassificationPolicy& policy,
             const FeatureSnapshot& features)
      : request_(request), policy_(policy), features_(features) {}

  CongestionAssessment run() {
    assessment_.subject = request_.subject;
    assessment_.window_start = request_.window_start;
    assessment_.window_end = request_.window_end;
    assessment_.evaluated_at = request_.evaluated_at;
    assessment_.generation = request_.generation;
    assessment_.tenant = request_.tenant;
    assessment_.features = features_;
    assessment_.explanation.policy_version = policy_.version;
    assessment_.explanation.policy_digest = policy_.digest();
    assessment_.explanation.policy_digest_hex = to_hex(assessment_.explanation.policy_digest);

    assess_rules();
    finalize();
    return assessment_;
  }

 private:
  void step(const char* rule_id, const char* decision, const char* reason_code,
            const std::vector<EvidenceId>& citations) {
    if (assessment_.explanation.steps.size() >= request_.max_citations) {
      return;
    }
    ExplanationStep step_value;
    step_value.rule_id = rule_id;
    step_value.decision = decision;
    step_value.reason_code = reason_code;
    step_value.citations = citations;
    std::sort(step_value.citations.begin(), step_value.citations.end());
    assessment_.explanation.steps.push_back(std::move(step_value));
  }

  void block(std::string code, std::string detail) {
    for (const Blocker& existing : assessment_.blockers) {
      if (existing.code == code) {
        return;
      }
    }
    if (assessment_.blockers.size() >= 32) {
      return;
    }
    assessment_.blockers.push_back(Blocker{std::move(code), std::move(detail)});
  }

  void assess_rules() {
    if (features_.records_in_window == 0) {
      assessment_.verdict = Verdict::kNoEvidence;
      block(std::string(blockers::kAbsenceOfEvidence), "no records were supplied for this subject");
      step("R00-no-evidence", "applied", "no_records", {});
      assessment_.confidence = 0;
      return;
    }
    step("R00-no-evidence", "skipped", "records_present", {});

    if (features_.records_generation_mismatch > 0) {
      char detail[128];
      std::snprintf(detail, sizeof(detail), "%zu of %zu records carry another generation",
                    features_.records_generation_mismatch, features_.records_in_window);
      block(std::string(blockers::kGenerationMismatch), detail);
      step("R01-generation", "applied", "generation_mismatch_reported", {});
      if (features_.records_generation_mismatch == features_.records_in_window) {
        assessment_.verdict = Verdict::kIndeterminate;
        assessment_.confidence = clamp_confidence(30);
        return;
      }
    } else {
      step("R01-generation", "skipped", "generation_consistent", {});
    }

    if (features_.unsupported_records > 0) {
      block(std::string(blockers::kUnsupportedEvidence),
            std::to_string(features_.unsupported_records) + " record(s) report unsupported data");
      step("R02-unsupported", "applied", "unsupported_evidence_reported", {});
      if (features_.unsupported_records == features_.records_in_window) {
        assessment_.verdict = Verdict::kIndeterminate;
        assessment_.confidence = clamp_confidence(25);
        return;
      }
    } else {
      step("R02-unsupported", "skipped", "all_records_supported", {});
    }

    if (!features_.any_fresh_deciding) {
      assessment_.verdict = Verdict::kIndeterminate;
      if (features_.impairment_seen_not_fresh) {
        block(std::string(blockers::kStaleImpairment),
              "impairment evidence exists but none of it is fresh");
        step("R03-freshness", "applied", "stale_impairment_only", {});
      } else if (features_.pressure_seen_not_fresh) {
        block(std::string(blockers::kStalePressureCannotProveCurrent),
              "pressure evidence exists but none of it is fresh");
        step("R03-freshness", "applied", "stale_pressure_only", {});
      } else if (features_.utilization_seen_not_fresh || features_.demand_seen_not_fresh ||
                 features_.contention_seen_not_fresh) {
        block(std::string(blockers::kUnknownFreshness),
              "records are present but none is usable as deciding evidence");
        step("R03-freshness", "applied", "no_fresh_deciding_evidence", {});
      } else {
        block(std::string(blockers::kNoFreshEvidence), "no record can support a current verdict");
        step("R03-freshness", "applied", "no_usable_evidence", {});
      }
      collect_conflict_blockers();
      assessment_.confidence = clamp_confidence(35);
      return;
    }
    step("R03-freshness", "applied", "fresh_deciding_evidence_present", {});

    const bool impairment_decisive = impairment_is_decisive();
    if (impairment_decisive) {
      assessment_.verdict = Verdict::kCongestionConfirmed;
      assessment_.mechanisms = features_.observed_mechanisms;
      assessment_.severity = severity_for_rates(features_, policy_);
      step("R04-impairment", "applied", "fresh_impairment_above_threshold", {});
      return;
    }
    if (features_.impairment_present) {
      if (features_.impairment_counter_without_delta && !features_.impairment_unquantified) {
        block(std::string(blockers::kCounterWithoutDelta),
              "only cumulative counters were supplied: a lifetime total cannot prove present harm");
      } else if (features_.impairment_below_threshold) {
        block(std::string(blockers::kImpairmentBelowThreshold),
              "fresh impairment evidence stays below the configured threshold");
      }
      step("R04-impairment", "applied", "impairment_not_decisive", {});
    } else {
      step("R04-impairment", "skipped", "no_fresh_impairment_evidence", {});
    }

    const bool demand_saturated =
        features_.capacity_known && features_.demand_exceeds_capacity && features_.demand_present;
    if (demand_saturated) {
      assessment_.verdict = Verdict::kSaturated;
      assessment_.severity = Severity::kModerate;
      step("R05-saturation", "applied", "offered_demand_meets_capacity", {});
      return;
    }
    step("R05-saturation", "skipped", "demand_below_capacity_or_unknown", {});

    // A backlog verdict needs a meaningful backlog: either occupancy above the policy ratio, or
    // occupancy that the source could not normalise. A nearly empty queue is not a backlog.
    const bool backlog_meaningful = features_.pressure_exceeds_ratio ||
                                    (features_.pressure_present && !features_.pressure_ratio_available);
    if (backlog_meaningful) {
      assessment_.verdict = Verdict::kPressureObserved;
      assessment_.severity = Severity::kLow;
      step("R06-pressure", "applied", "fresh_backlog_evidence", {});
      return;
    }
    if (features_.pressure_present) {
      block(std::string(blockers::kBacklogBelowThreshold),
            "backlog evidence is present but below the configured pressure ratio");
      step("R06-pressure", "applied", "backlog_below_policy_threshold", {});
    }
    if (features_.pressure_seen_not_fresh) {
      block(std::string(blockers::kStalePressureCannotProveCurrent),
            "backlog evidence exists but none of it is fresh");
      step("R06-pressure", "applied", "stale_pressure_not_usable", {});
    } else {
      step("R06-pressure", "skipped", "no_pressure_evidence", {});
    }

    if (features_.contention_present && features_.utilization_present &&
        features_.utilization_exceeds_attention) {
      assessment_.verdict = Verdict::kContentionObserved;
      assessment_.severity = Severity::kLow;
      step("R07-contention", "applied", "multiple_consumers_on_a_loaded_resource", {});
      return;
    }
    step("R07-contention", "skipped", "contention_not_established", {});

    if (features_.utilization_exceeds_high || features_.utilization_exceeds_attention) {
      assessment_.verdict = Verdict::kUtilizedHealthy;
      assessment_.severity = Severity::kInformational;
      block(std::string(blockers::kUtilizationAlone),
            "high utilization without impairment evidence is not congestion");
      step("R08-utilization", "applied", "utilization_high_without_impairment", {});
      return;
    }
    if (features_.utilization_present) {
      assessment_.verdict = Verdict::kIdle;
      assessment_.severity = Severity::kNone;
      step("R08-utilization", "applied", "utilization_below_attention", {});
      return;
    }
    assessment_.verdict = Verdict::kIdle;
    assessment_.severity = Severity::kNone;
    step("R09-idle", "applied", "no_load_evidence", {});
  }

  bool impairment_is_decisive() const {
    if (!features_.impairment_present) {
      return false;
    }
    if (features_.drop_rate >= policy_.drop_rate_threshold && features_.drop_rate > 0.0) {
      return true;
    }
    if (features_.mark_rate >= policy_.mark_rate_threshold && features_.mark_rate > 0.0) {
      return true;
    }
    if (features_.pause_rate >= policy_.pause_rate_threshold && features_.pause_rate > 0.0) {
      return true;
    }
    if (features_.retransmit_rate >= policy_.retransmit_rate_threshold &&
        features_.retransmit_rate > 0.0) {
      return true;
    }
    if (features_.latency_inflation_demonstrated) {
      return true;
    }
    return features_.impairment_unquantified;
  }

  void collect_conflict_blockers() {
    const struct {
      const char* role;
      Agreement agreement;
    } entries[] = {{"utilization", features_.utilization_agreement},
                   {"pressure", features_.pressure_agreement},
                   {"demand", features_.demand_agreement},
                   {"impairment", features_.impairment_agreement}};
    for (const auto& entry : entries) {
      if (agreement_is_conflict(entry.agreement)) {
        block(std::string(blockers::kConflictingSources),
              std::string(entry.role) + " sources disagree (" + std::string(to_string(entry.agreement)) +
                  "); every position stays cited");
        step("R10-agreement", "applied", "conflicting_sources_reported", {});
      }
    }
    if (features_.incomplete_records > 0) {
      block(std::string(blockers::kIncompleteEvidence),
            std::to_string(features_.incomplete_records) + " record(s) are incomplete");
    }
    if (features_.records_unknown_freshness > 0) {
      block(std::string(blockers::kUnknownFreshness),
            std::to_string(features_.records_unknown_freshness) +
                " record(s) have unknown freshness");
    }
    if (features_.records_recovered > 0) {
      block(std::string(blockers::kPersistedNotLive),
            std::to_string(features_.records_recovered) +
                " record(s) were recovered from a snapshot and are not live");
    }
    if (features_.records_retired > 0) {
      block(std::string(blockers::kRetiredIncarnation),
            std::to_string(features_.records_retired) +
                " record(s) belong to a retired source incarnation");
    }
  }

  void collect_citations() {
    const std::vector<const EvidenceRecord*>* buckets[] = {
        &utilization_records_, &pressure_records_, &contention_records_, &demand_records_,
        &capacity_records_, &impairment_records_};
    for (const auto* bucket : buckets) {
      for (const EvidenceRecord* record : *bucket) {
        assessment_.citations.push_back(record->id);
      }
    }
    std::sort(assessment_.citations.begin(), assessment_.citations.end());
    assessment_.citations.erase(std::unique(assessment_.citations.begin(), assessment_.citations.end()),
                                assessment_.citations.end());
    if (assessment_.citations.size() > request_.max_citations) {
      assessment_.citations.resize(request_.max_citations);
    }
  }

  std::uint32_t compute_confidence() const {
    const bool corroborated =
        features_.deciding_sources >= policy_.corroborating_source_target;
    const bool unanimous = features_.utilization_agreement == Agreement::kUnanimous ||
                           features_.impairment_agreement == Agreement::kUnanimous;
    const bool conflict = agreement_is_conflict(features_.utilization_agreement) ||
                          agreement_is_conflict(features_.pressure_agreement) ||
                          agreement_is_conflict(features_.demand_agreement) ||
                          agreement_is_conflict(features_.impairment_agreement);
    int value = 0;
    switch (assessment_.verdict) {
      case Verdict::kNoEvidence:
        return 0;
      case Verdict::kIdle:
        value = 40 + (features_.utilization_present ? 15 : 0) +
                (features_.demand_present ? 15 : 0) + (corroborated ? 15 : 0);
        break;
      case Verdict::kUtilizedHealthy:
        value = 45 + (corroborated ? 15 : 0) + (unanimous ? 10 : 0) +
                (features_.demand_present ? 15 : 0) - (conflict ? 15 : 0);
        break;
      case Verdict::kContentionObserved:
      case Verdict::kPressureObserved:
        value = 45 + (corroborated ? 20 : 0) + (features_.utilization_present ? 10 : 0) -
                (conflict ? 15 : 0);
        break;
      case Verdict::kSaturated:
        value = 50 + (corroborated ? 20 : 0) + (features_.utilization_present ? 10 : 0) -
                (conflict ? 15 : 0);
        break;
      case Verdict::kCongestionConfirmed:
        value = 50 + (corroborated ? 20 : 0) + (unanimous ? 10 : 0) +
                (features_.demand_present ? 10 : 0) + (features_.capacity_known ? 5 : 0) -
                (conflict ? 15 : 0);
        break;
      case Verdict::kIndeterminate:
        value = 30 + (conflict ? 20 : 0) + (features_.records_in_window > 0 ? 10 : 0) +
                (features_.impairment_counter_without_delta ? 10 : 0);
        break;
      case Verdict::kCount:
      default:
        value = 0;
        break;
    }
    return clamp_confidence(value);
  }

  void finalize() {
    collect_citations();
    collect_conflict_blockers();
    if (features_.deciding_sources <= 1 && features_.records_in_window > 0) {
      block(std::string(blockers::kSingleSourceUncorroborated),
            "no independent source corroborates the deciding evidence");
    }
    if ((assessment_.verdict == Verdict::kCongestionConfirmed ||
         assessment_.verdict == Verdict::kSaturated) &&
        !features_.capacity_known) {
      block(std::string(blockers::kNoDemandContext),
            "capacity is unknown, so demand cannot be related to serviceable bandwidth");
    }
    if (!features_.any_fresh_deciding && features_.records_in_window > 0 &&
        assessment_.verdict != Verdict::kIndeterminate) {
      block(std::string(blockers::kNoFreshEvidence), "no fresh deciding evidence is available");
    }
    assessment_.confidence = compute_confidence();
    assessment_.deciding_agreement = features_.impairment_agreement != Agreement::kUnknown
                                         ? features_.impairment_agreement
                                         : features_.utilization_agreement;
    assessment_.sources = features_.deciding_source_list;
    assessment_.congestion_asserted = is_congestion_assertion(assessment_.verdict);
    assessment_.basis.deciding_records = assessment_.citations.size();
    assessment_.basis.corroborating_sources = features_.deciding_sources;
    assessment_.basis.fresh_records = features_.records_fresh;
    assessment_.basis.aging_records = features_.records_aging;
    assessment_.basis.stale_records = features_.records_stale;
    assessment_.basis.expired_records = features_.records_expired;
    assessment_.basis.unknown_freshness_records = features_.records_unknown_freshness;
    assessment_.basis.unsupported_records = features_.unsupported_records;
    assessment_.basis.incomplete_records = features_.incomplete_records;
    assessment_.basis.conflicting_features = static_cast<std::size_t>(
        (agreement_is_conflict(features_.utilization_agreement) ? 1 : 0) +
        (agreement_is_conflict(features_.pressure_agreement) ? 1 : 0) +
        (agreement_is_conflict(features_.demand_agreement) ? 1 : 0) +
        (agreement_is_conflict(features_.impairment_agreement) ? 1 : 0));
    assessment_.basis.raw_points = assessment_.confidence;
    assessment_.explanation.blockers = assessment_.blockers;
  }

  const ClassificationRequest& request_;
  const ClassificationPolicy& policy_;
  const FeatureSnapshot& features_;
  CongestionAssessment assessment_{};

 public:
  // Records that contributed, kept for citation collection.
  std::vector<const EvidenceRecord*> utilization_records_{};
  std::vector<const EvidenceRecord*> pressure_records_{};
  std::vector<const EvidenceRecord*> contention_records_{};
  std::vector<const EvidenceRecord*> demand_records_{};
  std::vector<const EvidenceRecord*> capacity_records_{};
  std::vector<const EvidenceRecord*> impairment_records_{};
  void attach_records(std::vector<const EvidenceRecord*> utilization,
                      std::vector<const EvidenceRecord*> pressure,
                      std::vector<const EvidenceRecord*> contention,
                      std::vector<const EvidenceRecord*> demand,
                      std::vector<const EvidenceRecord*> capacity,
                      std::vector<const EvidenceRecord*> impairment) {
    utilization_records_ = std::move(utilization);
    pressure_records_ = std::move(pressure);
    contention_records_ = std::move(contention);
    demand_records_ = std::move(demand);
    capacity_records_ = std::move(capacity);
    impairment_records_ = std::move(impairment);
  }
};

}  // namespace

std::string_view to_string(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::kNoEvidence: return "no_evidence";
    case Verdict::kIdle: return "idle";
    case Verdict::kUtilizedHealthy: return "utilized_healthy";
    case Verdict::kContentionObserved: return "contention_observed";
    case Verdict::kPressureObserved: return "pressure_observed";
    case Verdict::kSaturated: return "saturated";
    case Verdict::kCongestionConfirmed: return "congestion_confirmed";
    case Verdict::kIndeterminate: return "indeterminate";
    case Verdict::kCount: break;
  }
  return "indeterminate";
}

bool is_congestion_assertion(Verdict verdict) noexcept {
  return verdict == Verdict::kCongestionConfirmed;
}

std::string_view to_string(Mechanism mechanism) noexcept {
  switch (mechanism) {
    case Mechanism::kNone: return "none";
    case Mechanism::kPacketDrop: return "packet_drop";
    case Mechanism::kEcnMarking: return "ecn_marking";
    case Mechanism::kPauseBackpressure: return "pause_backpressure";
    case Mechanism::kLatencyInflation: return "latency_inflation";
    case Mechanism::kRetransmission: return "retransmission";
    case Mechanism::kCount: break;
  }
  return "none";
}

bool has_mechanism(MechanismSet set, Mechanism mechanism) noexcept {
  return (set & mechanism_bit(mechanism)) != 0;
}

std::string describe_mechanisms(MechanismSet set) {
  std::string out;
  for (int i = static_cast<int>(Mechanism::kPacketDrop); i < static_cast<int>(Mechanism::kCount);
       ++i) {
    const auto mechanism = static_cast<Mechanism>(i);
    if (has_mechanism(set, mechanism)) {
      if (!out.empty()) {
        out.push_back(',');
      }
      out += to_string(mechanism);
    }
  }
  return out.empty() ? std::string("none") : out;
}

Mechanism primary_mechanism(MechanismSet set) noexcept {
  for (int i = static_cast<int>(Mechanism::kPacketDrop); i < static_cast<int>(Mechanism::kCount);
       ++i) {
    const auto mechanism = static_cast<Mechanism>(i);
    if (has_mechanism(set, mechanism)) {
      return mechanism;
    }
  }
  return Mechanism::kNone;
}

std::string_view to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::kNone: return "none";
    case Severity::kInformational: return "informational";
    case Severity::kLow: return "low";
    case Severity::kModerate: return "moderate";
    case Severity::kHigh: return "high";
    case Severity::kCritical: return "critical";
  }
  return "none";
}

bool Explanation::has_blocker(std::string_view code) const noexcept {
  for (const Blocker& blocker : blockers) {
    if (blocker.code == code) {
      return true;
    }
  }
  return false;
}

std::string Explanation::render() const {
  std::string out;
  out += "policy=";
  out += policy_version;
  out += " digest=";
  out += policy_digest_hex;
  out += "\n";
  for (const ExplanationStep& entry : steps) {
    out += "  rule ";
    out += entry.rule_id;
    out += ": ";
    out += entry.decision;
    out += " (";
    out += entry.reason_code;
    out += ")";
    if (!entry.citations.empty()) {
      out += " citations=";
      for (std::size_t i = 0; i < entry.citations.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out += entry.citations[i].str();
      }
    }
    out.push_back('\n');
  }
  for (const Blocker& blocker : blockers) {
    out += "  blocker ";
    out += blocker.code;
    out += ": ";
    out += blocker.detail;
    out.push_back('\n');
  }
  return out;
}

bool CongestionAssessment::has_blocker(std::string_view code) const noexcept {
  for (const Blocker& blocker : blockers) {
    if (blocker.code == code) {
      return true;
    }
  }
  return false;
}

FeatureSnapshot extract_features(const ClassificationRequest& request,
                                 const std::vector<const EvidenceRecord*>& records,
                                 const ClassificationPolicy& policy) {
  FeatureSnapshot features;
  features.subject = request.subject;
  features.records_considered = records.size();

  std::vector<const EvidenceRecord*> fresh_utilization;
  std::vector<const EvidenceRecord*> fresh_pressure;
  std::vector<const EvidenceRecord*> fresh_contention;
  std::vector<const EvidenceRecord*> fresh_demand;
  std::vector<const EvidenceRecord*> fresh_capacity;
  std::vector<const EvidenceRecord*> fresh_impairment;
  std::vector<const EvidenceRecord*> fresh_latency;
  std::set<Name> deciding_sources;

  features.utilization_freshness = Freshness::kUnknown;
  features.pressure_freshness = Freshness::kUnknown;
  features.contention_freshness = Freshness::kUnknown;
  features.demand_freshness = Freshness::kUnknown;
  features.impairment_freshness = Freshness::kUnknown;

  for (const EvidenceRecord* record : records) {
    if (record == nullptr) {
      continue;
    }
    if (record->subject != request.subject) {
      continue;
    }
    ++features.records_in_window;
    const FreshnessAssessment freshness =
        assess_freshness(*record, request.evaluated_at, policy.freshness);
    switch (freshness.freshness) {
      case Freshness::kFresh: ++features.records_fresh; break;
      case Freshness::kAging: ++features.records_aging; break;
      case Freshness::kStale: ++features.records_stale; break;
      case Freshness::kExpired: ++features.records_expired; break;
      case Freshness::kUnknown: ++features.records_unknown_freshness; break;
    }
    if (record->recovered_from_snapshot) {
      ++features.records_recovered;
    }
    if (record->retired) {
      ++features.records_retired;
    }
    if (record->support == Support::kUnsupported) {
      features.unsupported_evidence_seen = true;
      ++features.unsupported_records;
    }
    if (record->completeness != Completeness::kComplete) {
      features.incomplete_evidence_seen = true;
      ++features.incomplete_records;
    }
    if (!generation_matches(*record, request.generation)) {
      ++features.records_generation_mismatch;
      continue;
    }

    const bool fresh = usable_as_deciding_evidence(freshness.freshness) &&
                       record->support != Support::kUnsupported;
    const EvidenceRole role = role_of(record->kind);
    if (fresh) {
      deciding_sources.insert(record->provenance.source.name());
      features.any_fresh_deciding = true;
    }
    switch (role) {
      case EvidenceRole::kUtilization:
        features.utilization_freshness =
            best_freshness(features.utilization_freshness, freshness.freshness);
        if (fresh) {
          fresh_utilization.push_back(record);
        } else {
          features.utilization_seen_not_fresh = true;
        }
        break;
      case EvidenceRole::kPressure:
        features.pressure_freshness =
            best_freshness(features.pressure_freshness, freshness.freshness);
        if (fresh) {
          fresh_pressure.push_back(record);
        } else {
          features.pressure_seen_not_fresh = true;
        }
        break;
      case EvidenceRole::kContention:
        features.contention_freshness =
            best_freshness(features.contention_freshness, freshness.freshness);
        if (fresh) {
          fresh_contention.push_back(record);
        } else {
          features.contention_seen_not_fresh = true;
        }
        break;
      case EvidenceRole::kDemand:
        features.demand_freshness = best_freshness(features.demand_freshness, freshness.freshness);
        if (fresh) {
          fresh_demand.push_back(record);
        } else {
          features.demand_seen_not_fresh = true;
        }
        break;
      case EvidenceRole::kCapacity:
        if (fresh) {
          fresh_capacity.push_back(record);
        }
        break;
      case EvidenceRole::kImpairment:
        features.impairment_freshness =
            best_freshness(features.impairment_freshness, freshness.freshness);
        if (fresh) {
          fresh_impairment.push_back(record);
          if (record->kind == EvidenceKind::kLatencySample) {
            fresh_latency.push_back(record);
          }
        } else {
          features.impairment_seen_not_fresh = true;
        }
        break;
      case EvidenceRole::kTopology:
      case EvidenceRole::kLiveness:
      case EvidenceRole::kUnknown:
      default:
        break;
    }
  }

  features.deciding_sources = deciding_sources.size();
  features.deciding_source_list.reserve(deciding_sources.size());
  for (const Name& source : deciding_sources) {
    features.deciding_source_list.emplace_back(source);
  }

  if (!fresh_utilization.empty()) {
    features.utilization_present = true;
    features.utilization = max_scalar(fresh_utilization);
    features.utilization_exceeds_attention =
        features.utilization >= policy.utilization_attention;
    features.utilization_exceeds_high = features.utilization >= policy.utilization_high;
    features.utilization_agreement =
        assess_agreement(fresh_utilization, policy.conflict_relative_tolerance, request.max_citations)
            .agreement;
  }
  if (!fresh_pressure.empty()) {
    features.pressure_present = true;
    features.pressure_ratio_available = any_ratio_unit(fresh_pressure);
    features.pressure_ratio = max_ratio(fresh_pressure);
    features.pressure_exceeds_ratio =
        features.pressure_ratio_available && features.pressure_ratio >= policy.pressure_occupancy_ratio;
    features.pressure_agreement =
        assess_agreement(fresh_pressure, policy.conflict_relative_tolerance, request.max_citations)
            .agreement;
  }
  if (!fresh_contention.empty()) {
    features.contention_present = true;
    features.contention_degree = max_scalar(fresh_contention);
  }
  if (!fresh_demand.empty()) {
    features.demand_present = true;
    double offered = 0.0;
    double achieved = 0.0;
    for (const EvidenceRecord* record : fresh_demand) {
      if (record->kind == EvidenceKind::kOfferedDemand && record->value.scalar > offered) {
        offered = record->value.scalar;
      }
      if (record->kind == EvidenceKind::kAchievedThroughput && record->value.scalar > achieved) {
        achieved = record->value.scalar;
      }
    }
    features.offered_bps = offered;
    features.achieved_bps = achieved;
    features.demand_agreement =
        assess_agreement(fresh_demand, policy.conflict_relative_tolerance, request.max_citations)
            .agreement;
  }
  if (!fresh_capacity.empty()) {
    features.capacity_bps = max_scalar(fresh_capacity);
    features.capacity_known = features.capacity_bps > 0.0;
  }
  if (features.capacity_known && features.offered_bps > 0.0) {
    features.demand_capacity_ratio = features.offered_bps / features.capacity_bps;
    features.demand_exceeds_capacity =
        features.demand_capacity_ratio >= policy.demand_capacity_ratio;
  }

  if (!fresh_impairment.empty()) {
    features.impairment_present = true;
    const RateObservation drops = observe_rate(fresh_impairment, EvidenceKind::kDropCount);
    const RateObservation marks = observe_rate(fresh_impairment, EvidenceKind::kMarkCount);
    const RateObservation pauses = observe_rate(fresh_impairment, EvidenceKind::kPauseCount);
    const RateObservation retransmits =
        observe_rate(fresh_impairment, EvidenceKind::kRetransmitCount);
    features.drop_rate = drops.rate;
    features.mark_rate = marks.rate;
    features.pause_rate = pauses.rate;
    features.retransmit_rate = retransmits.rate;
    features.impairment_unquantified =
        drops.unquantified || marks.unquantified || pauses.unquantified || retransmits.unquantified;
    features.impairment_counter_without_delta = drops.counter_without_delta ||
                                                marks.counter_without_delta ||
                                                pauses.counter_without_delta ||
                                                retransmits.counter_without_delta;
    if (drops.observed) {
      features.observed_mechanisms |= mechanism_bit(Mechanism::kPacketDrop);
    }
    if (marks.observed) {
      features.observed_mechanisms |= mechanism_bit(Mechanism::kEcnMarking);
    }
    if (pauses.observed) {
      features.observed_mechanisms |= mechanism_bit(Mechanism::kPauseBackpressure);
    }
    if (retransmits.observed) {
      features.observed_mechanisms |= mechanism_bit(Mechanism::kRetransmission);
    }

    // Latency inflation requires a baseline: a single sample cannot demonstrate a change.
    double minimum_latency = 0.0;
    double maximum_latency = 0.0;
    std::size_t latency_samples = 0;
    for (const EvidenceRecord* record : fresh_latency) {
      const double value = record->value.scalar;
      if (value <= 0.0) {
        continue;
      }
      if (latency_samples == 0 || value < minimum_latency) {
        minimum_latency = value;
      }
      if (latency_samples == 0 || value > maximum_latency) {
        maximum_latency = value;
      }
      ++latency_samples;
    }
    if (latency_samples >= 2 && minimum_latency > 0.0) {
      features.latency_factor = maximum_latency / minimum_latency;
      features.latency_inflation_demonstrated =
          features.latency_factor >= policy.latency_inflation_factor;
    }
    if (features.latency_inflation_demonstrated) {
      features.observed_mechanisms |= mechanism_bit(Mechanism::kLatencyInflation);
    }
    features.impairment_below_threshold =
        !(features.drop_rate >= policy.drop_rate_threshold && features.drop_rate > 0.0) &&
        !(features.mark_rate >= policy.mark_rate_threshold && features.mark_rate > 0.0) &&
        !(features.pause_rate >= policy.pause_rate_threshold && features.pause_rate > 0.0) &&
        !(features.retransmit_rate >= policy.retransmit_rate_threshold &&
          features.retransmit_rate > 0.0) &&
        !features.latency_inflation_demonstrated && !features.impairment_unquantified;
    features.impairment_agreement =
        assess_agreement(fresh_impairment, policy.conflict_relative_tolerance, request.max_citations)
            .agreement;
  }

  StableHasher hasher;
  hasher.update_u64(static_cast<std::uint64_t>(features.records_in_window));
  hasher.update_u64(static_cast<std::uint64_t>(features.records_fresh));
  hasher.update_u64(static_cast<std::uint64_t>(features.records_stale));
  hasher.update_u64(static_cast<std::uint64_t>(features.records_expired));
  hasher.update_u64(static_cast<std::uint64_t>(features.records_generation_mismatch));
  hasher.update_bool(features.utilization_present);
  hash_feature_value(hasher, features.utilization);
  hasher.update_bool(features.pressure_present);
  hash_feature_value(hasher, features.pressure_ratio);
  hasher.update_bool(features.contention_present);
  hash_feature_value(hasher, features.contention_degree);
  hasher.update_bool(features.demand_present);
  hash_feature_value(hasher, features.demand_capacity_ratio);
  hasher.update_bool(features.impairment_present);
  hash_feature_value(hasher, features.drop_rate);
  hash_feature_value(hasher, features.mark_rate);
  hash_feature_value(hasher, features.pause_rate);
  hash_feature_value(hasher, features.latency_factor);
  hasher.update_u64(features.observed_mechanisms);
  features.feature_digest = hasher.digest64();
  return features;
}

Result<CongestionAssessment> classify_subject(const ClassificationRequest& request,
                                              const std::vector<const EvidenceRecord*>& records,
                                              const ClassificationPolicy& policy) {
  if (!request.subject.valid()) {
    return make_error(ErrorCode::kInvalidArgument, "classification requires a subject");
  }
  if (request.window_end < request.window_start) {
    return make_error(ErrorCode::kInvalidArgument, "classification window ends before it starts");
  }
  FeatureSnapshot features = extract_features(request, records, policy);

  std::vector<const EvidenceRecord*> fresh_utilization;
  std::vector<const EvidenceRecord*> fresh_pressure;
  std::vector<const EvidenceRecord*> fresh_contention;
  std::vector<const EvidenceRecord*> fresh_demand;
  std::vector<const EvidenceRecord*> fresh_capacity;
  std::vector<const EvidenceRecord*> fresh_impairment;
  for (const EvidenceRecord* record : records) {
    if (record == nullptr || record->subject != request.subject) {
      continue;
    }
    if (!generation_matches(*record, request.generation)) {
      continue;
    }
    const FreshnessAssessment freshness =
        assess_freshness(*record, request.evaluated_at, policy.freshness);
    if (!usable_as_deciding_evidence(freshness.freshness) ||
        record->support == Support::kUnsupported) {
      continue;
    }
    switch (role_of(record->kind)) {
      case EvidenceRole::kUtilization: fresh_utilization.push_back(record); break;
      case EvidenceRole::kPressure: fresh_pressure.push_back(record); break;
      case EvidenceRole::kContention: fresh_contention.push_back(record); break;
      case EvidenceRole::kDemand: fresh_demand.push_back(record); break;
      case EvidenceRole::kCapacity: fresh_capacity.push_back(record); break;
      case EvidenceRole::kImpairment: fresh_impairment.push_back(record); break;
      default: break;
    }
  }

  RuleEngine engine(request, policy, features);
  engine.attach_records(std::move(fresh_utilization), std::move(fresh_pressure),
                        std::move(fresh_contention), std::move(fresh_demand),
                        std::move(fresh_capacity), std::move(fresh_impairment));
  return engine.run();
}

}  // namespace congestion
