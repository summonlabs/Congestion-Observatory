// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/localization/localize.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

namespace congestion {
namespace {

struct TraversalStep {
  EvidenceSubject node{};
  std::size_t depth{0};
  CausalEdgeKind incoming_kind{CausalEdgeKind::kTopologyAdjacency};
  std::string incoming_rule{};
  std::uint32_t incoming_strength{0};
  bool incoming_observational{false};
  std::vector<EvidenceId> citations{};
};

struct NodeScore {
  EvidenceSubject subject{};
  std::uint32_t score{0};
  std::size_t depth{0};
  std::vector<EvidenceId> citations{};
  std::vector<std::string> rules{};
  std::vector<std::string> basis{};
  bool decisive{false};
  std::size_t distinct_sources{0};
  bool fresh_evidence{false};
};

void add_basis(NodeScore& score, const char* name, std::uint32_t points) {
  if (points == 0) {
    return;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s(+%u)", name, points);
  score.basis.emplace_back(buffer);
  score.score += points;
}

void accumulate(std::vector<EvidenceId>& target, const std::vector<EvidenceId>& source,
                std::size_t limit) {
  for (const EvidenceId& id : source) {
    if (target.size() >= limit) {
      return;
    }
    target.push_back(id);
  }
}

}  // namespace

std::string_view to_string(LocalizationOutcome outcome) noexcept {
  switch (outcome) {
    case LocalizationOutcome::kLocalized: return "localized";
    case LocalizationOutcome::kAmbiguous: return "ambiguous";
    case LocalizationOutcome::kUnlocalized: return "unlocalized";
    case LocalizationOutcome::kNotAttempted: return "not_attempted";
  }
  return "not_attempted";
}

Result<LocalizationResult> localize(const LocalizationRequest& request, const CausalGraph& graph,
                                    const std::vector<const EvidenceRecord*>& records,
                                    const ClassificationPolicy& policy,
                                    const LocalizationPolicy& localization_policy,
                                    const Limits& limits) {
  LocalizationResult result;
  result.symptom = request.symptom;
  result.explanation.policy_version = policy.version;
  result.explanation.policy_digest = policy.digest();
  result.explanation.policy_digest_hex = to_hex(result.explanation.policy_digest);

  if (!request.symptom.valid()) {
    return make_error(ErrorCode::kInvalidArgument, "localization requires a symptom subject");
  }
  if (request.assessment == nullptr || !request.assessment->congestion_asserted) {
    result.outcome = LocalizationOutcome::kNotAttempted;
    result.blockers.push_back(Blocker{std::string(localization_blockers::kSymptomNotAsserted),
                                      "the symptom is not an asserted congestion"});
    result.explanation.blockers = result.blockers;
    return result;
  }
  if (graph.edge_count() == 0) {
    result.outcome = LocalizationOutcome::kUnlocalized;
    result.blockers.push_back(Blocker{std::string(localization_blockers::kGraphEmpty),
                                      "no causal edges are known for this generation"});
    result.explanation.blockers = result.blockers;
    return result;
  }

  const std::size_t max_depth = std::min(localization_policy.max_depth, limits.max_localization_depth);
  const std::size_t max_candidates =
      std::min(localization_policy.max_candidates, limits.max_result_candidates);

  std::set<Name> visited;
  std::map<Name, std::vector<TraversalStep>> reached;
  std::deque<TraversalStep> queue;

  visited.insert(Name::unchecked(request.symptom.str()));
  queue.push_back(TraversalStep{request.symptom, 0, CausalEdgeKind::kTopologyAdjacency, {}, 0, false,
                                {}});

  while (!queue.empty()) {
    TraversalStep current = queue.front();
    queue.pop_front();
    if (current.depth >= max_depth) {
      // Edges point from a cause to the symptom it explains, so the search walks in-edges.
      const auto edges = graph.in_edges(current.node);
      if (!edges.empty()) {
        result.depth_limited = true;
      }
      continue;
    }
    const auto edges = graph.in_edges(current.node);
    for (const CausalEdge& edge : edges) {
      ++result.paths_examined;
      const Name key = Name::unchecked(edge.from.str());
      if (visited.find(key) != visited.end()) {
        continue;
      }
      visited.insert(key);
      TraversalStep next;
      next.node = edge.from;
      next.depth = current.depth + 1;
      next.incoming_kind = edge.kind;
      next.incoming_rule = edge.rule_id;
      next.incoming_strength = edge.strength;
      next.incoming_observational = edge.observational;
      next.citations = edge.citations;
      reached[key].push_back(next);
      queue.push_back(next);
    }
  }

  std::vector<NodeScore> candidates;
  for (const auto& entry : reached) {
    const TraversalStep& step = entry.second.front();
    NodeScore score;
    score.subject = step.node;
    score.depth = step.depth;
    score.rules.push_back(step.incoming_rule);
    accumulate(score.citations, step.citations, request.assessment->citations.size() + 1);

    std::set<Name> sources;
    bool demand_present = false;
    bool demand_exceeds = false;
    bool pressure_present = false;
    double pressure_ratio = 0.0;
    bool buffer_pressure = false;
    bool contention_present = false;
    bool impairment_present = false;
    bool any_fresh = false;
    double offered = 0.0;
    double capacity = 0.0;

    for (const EvidenceRecord* record : records) {
      if (record == nullptr || record->subject != step.node) {
        continue;
      }
      if (record->observed_at < request.window_start || record->observed_at > request.window_end) {
        continue;
      }
      if (record->recovered_from_snapshot || record->retired) {
        continue;
      }
      if (record->support == Support::kUnsupported) {
        continue;
      }
      const FreshnessAssessment freshness =
          assess_freshness(*record, request.evaluated_at, policy.freshness);
      if (!usable_as_deciding_evidence(freshness.freshness)) {
        continue;
      }
      any_fresh = true;
      sources.insert(record->provenance.source.name());
      accumulate(score.citations, {record->id}, limits.max_citations);
      switch (role_of(record->kind)) {
        case EvidenceRole::kDemand:
          demand_present = true;
          if (record->kind == EvidenceKind::kOfferedDemand && record->value.scalar > offered) {
            offered = record->value.scalar;
          }
          break;
        case EvidenceRole::kCapacity:
          if (record->value.scalar > capacity) {
            capacity = record->value.scalar;
          }
          break;
        case EvidenceRole::kPressure:
          pressure_present = true;
          if (record->kind == EvidenceKind::kBufferOccupancy) {
            buffer_pressure = true;
          }
          if (record->value.unit == ObservationUnit::kRatio && record->value.scalar > pressure_ratio) {
            pressure_ratio = record->value.scalar;
          }
          break;
        case EvidenceRole::kContention: contention_present = true; break;
        case EvidenceRole::kImpairment: impairment_present = true; break;
        default: break;
      }
    }

    if (capacity > 0.0 && offered > 0.0) {
      demand_exceeds = (offered / capacity) >= policy.demand_capacity_ratio;
    }
    score.distinct_sources = sources.size();
    score.fresh_evidence = any_fresh;
    score.decisive = any_fresh && (demand_present || pressure_present || contention_present ||
                                   impairment_present);

    if (localization_policy.require_decisive_evidence && !score.decisive) {
      // Structural adjacency alone never produces a culprit.
      result.explanation.steps.push_back(ExplanationStep{
          "L00-structural-only", "rejected", "no_decisive_evidence_at_candidate",
          score.citations});
      continue;
    }

    add_basis(score, "fresh_evidence", any_fresh ? 10u : 0u);
    add_basis(score, "demand_observed", demand_present ? 10u : 0u);
    add_basis(score, "demand_exceeds_capacity", demand_exceeds ? 25u : 0u);
    add_basis(score, "backlog_present", pressure_present ? 20u : 0u);
    add_basis(score, "backlog_ratio_high",
              (pressure_present && pressure_ratio >= policy.pressure_occupancy_ratio) ? 15u : 0u);
    add_basis(score, "buffer_pressure", buffer_pressure ? 10u : 0u);
    add_basis(score, "contention", contention_present ? 10u : 0u);
    add_basis(score, "impairment_upstream", impairment_present ? 5u : 0u);
    add_basis(score, "observational_edge", step.incoming_observational ? 10u : 0u);
    add_basis(score, "proximity", static_cast<std::uint32_t>(step.depth == 0 ? 0 : (5u > step.depth ? 5u - static_cast<std::uint32_t>(step.depth) : 0u)));
    if (score.score > 100u) {
      score.score = 100u;
    }
    candidates.push_back(std::move(score));
  }

  // Deterministic ranking: score descending, then identity ascending.
  std::sort(candidates.begin(), candidates.end(),
            [](const NodeScore& lhs, const NodeScore& rhs) {
              if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
              }
              return lhs.subject < rhs.subject;
            });
  if (candidates.size() > max_candidates) {
    candidates.resize(max_candidates);
  }

  std::vector<NodeScore> qualifying;
  for (NodeScore& candidate : candidates) {
    if (candidate.score >= localization_policy.minimum_candidate_score) {
      qualifying.push_back(std::move(candidate));
    }
  }

  if (qualifying.empty()) {
    result.outcome = LocalizationOutcome::kUnlocalized;
    result.blockers.push_back(Blocker{std::string(localization_blockers::kNoEvidenceBackedPath),
                                      "no candidate carried decisive evidence"});
    result.explanation.blockers = result.blockers;
    return result;
  }

  std::vector<NodeScore> reported;
  if (qualifying.size() >= 2 &&
      (qualifying[0].score - qualifying[1].score) < localization_policy.ambiguity_margin) {
    result.outcome = LocalizationOutcome::kAmbiguous;
    result.blockers.push_back(Blocker{
        std::string(localization_blockers::kAmbiguousCandidates),
        "the leading candidates are separated by less than the ambiguity margin"});
    for (const NodeScore& candidate : qualifying) {
      if (qualifying[0].score - candidate.score < localization_policy.ambiguity_margin) {
        reported.push_back(candidate);
      }
    }
  } else {
    result.outcome = LocalizationOutcome::kLocalized;
    reported.push_back(qualifying.front());
    for (std::size_t i = 1; i < qualifying.size(); ++i) {
      reported.push_back(qualifying[i]);
    }
  }

  for (const NodeScore& candidate : reported) {
    LocalizationCandidate out;
    out.subject = candidate.subject;
    out.score = candidate.score;
    out.depth = candidate.depth;
    out.citations = candidate.citations;
    std::sort(out.citations.begin(), out.citations.end());
    out.citations.erase(std::unique(out.citations.begin(), out.citations.end()), out.citations.end());
    if (out.citations.size() > limits.max_citations) {
      out.citations.resize(limits.max_citations);
    }
    out.rules = candidate.rules;
    out.basis = candidate.basis;
    // Single source evidence is weaker: the confidence is scaled down accordingly.
    out.confidence = candidate.distinct_sources >= 2 ? candidate.score
                                                     : static_cast<std::uint32_t>(
                                                           (candidate.score * 7u) / 10u);
    result.candidates.push_back(std::move(out));
  }

  if (result.depth_limited) {
    result.blockers.push_back(Blocker{std::string(localization_blockers::kDepthLimited),
                                      "the search stopped at the configured depth"});
  }
  if (result.outcome == LocalizationOutcome::kLocalized) {
    result.explanation.steps.push_back(
        ExplanationStep{"L01-localized", "applied", "single_dominant_candidate",
                        result.candidates.front().citations});
  }
  result.explanation.blockers = result.blockers;
  return result;
}

}  // namespace congestion
