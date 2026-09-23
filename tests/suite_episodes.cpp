// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Episode lifecycle, causal localization and correlation grouping.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <set>

using namespace congestion;

namespace {

EvidenceRecord edge_citation(const std::string& source_name, const GenerationVector& generation,
                             const EvidenceSubject& subject) {
  cotest::Source source = cotest::make_source(source_name, generation);
  return source.ratio(EvidenceKind::kTopologyAdvertisement, subject, 1.0, cotest::at(0));
}

ClassificationRequest request_for(const EvidenceSubject& subject, const GenerationVector& generation,
                                  std::int64_t now_offset = 1) {
  ClassificationRequest request;
  request.subject = subject;
  request.window_start = cotest::at(now_offset - 60);
  request.window_end = cotest::at(now_offset);
  request.evaluated_at = cotest::at(now_offset);
  request.generation = generation;
  request.max_citations = 64;
  return request;
}

}  // namespace

CO_TEST(localize_episode, causal_edges_always_require_evidence) {
  const Limits limits = cotest::test_limits();
  CausalGraph graph;
  CausalEdge edge;
  edge.from = cotest::link_subject("a");
  edge.to = cotest::link_subject("b");
  edge.rule_id = "rule-1";
  CO_EXPECT_STATUS_ERR(graph.add_edge(edge, limits), ErrorCode::kPreconditionFailed);
  CO_EXPECT_EQ(graph.edge_count(), static_cast<std::size_t>(0));

  edge.rule_id.clear();
  edge.citations.push_back(EvidenceId::from_digest({1, 2}));
  CO_EXPECT_STATUS_ERR(graph.add_edge(edge, limits), ErrorCode::kInvalidArgument);

  edge.rule_id = "rule-1";
  edge.to = EvidenceSubject{};
  CO_EXPECT_STATUS_ERR(graph.add_edge(edge, limits), ErrorCode::kInvalidArgument);

  edge.to = cotest::link_subject("b");
  CO_EXPECT(graph.add_edge(edge, limits).ok());
  CO_EXPECT_EQ(graph.edge_count(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(graph.node_count(), static_cast<std::size_t>(2));
  CO_EXPECT(graph.find_edge(cotest::link_subject("a"), cotest::link_subject("b")) != nullptr);
  CO_EXPECT_EQ(graph.out_edges(cotest::link_subject("a")).size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(graph.in_edges(cotest::link_subject("b")).size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(graph.in_edges(cotest::link_subject("a")).size(), static_cast<std::size_t>(0));

  // Identical edges are idempotent; the digest is stable.
  const std::uint64_t digest = graph.digest();
  CO_EXPECT(graph.add_edge(edge, limits).ok());
  CO_EXPECT_EQ(graph.digest(), digest);
}

CO_TEST(localize_episode, localization_requires_an_asserted_symptom) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  CausalGraph graph;
  CausalEdge edge;
  edge.from = cotest::link_subject("upstream");
  edge.to = cotest::link_subject("symptom");
  edge.rule_id = "rule-1";
  edge.citations.push_back(EvidenceId::from_digest({1, 2}));
  edge.generation = generation;
  CO_EXPECT(graph.add_edge(edge, limits).ok());

  LocalizationRequest request;
  request.symptom = cotest::link_subject("symptom");
  request.window_start = cotest::at(-10);
  request.window_end = cotest::at(10);
  request.evaluated_at = cotest::at(0);
  request.generation = generation;
  request.assessment = nullptr;

  auto result = localize(request, graph, {}, ClassificationPolicy{}, LocalizationPolicy{}, limits);
  CO_EXPECT_OK(result);
  CO_EXPECT_EQ(result.value().outcome, LocalizationOutcome::kNotAttempted);
  CO_EXPECT(!result.value().blockers.empty());
}

CO_TEST(localize_episode, structural_adjacency_alone_is_never_a_culprit) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  const EvidenceSubject symptom = cotest::link_subject("symptom");
  const EvidenceSubject upstream = cotest::link_subject("upstream");

  CausalGraph graph;
  CausalEdge edge;
  edge.from = upstream;
  edge.to = symptom;
  edge.rule_id = "topology-adjacency";
  edge.citations.push_back(EvidenceId::from_digest({7, 8}));
  edge.generation = generation;
  CO_EXPECT(graph.add_edge(edge, limits).ok());

  // The symptom itself is congested; the upstream node has no decisive evidence of its own.
  cotest::Source source = cotest::make_source("collector-1", generation);
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, symptom, 0.01, cotest::at(0)));
  auto symptom_assessment = classify_subject(request_for(symptom, generation),
                                             cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(symptom_assessment);
  CO_EXPECT(symptom_assessment.value().congestion_asserted);

  LocalizationRequest request;
  request.symptom = symptom;
  request.window_start = cotest::at(-60);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;
  request.assessment = &symptom_assessment.value();

  auto result = localize(request, graph, cotest::pointers_to(records), ClassificationPolicy{},
                         LocalizationPolicy{}, limits);
  CO_EXPECT_OK(result);
  CO_EXPECT_EQ(result.value().outcome, LocalizationOutcome::kUnlocalized);
  CO_EXPECT(result.value().candidates.empty());
}

CO_TEST(localize_episode, localization_finds_the_evidence_backed_cause) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  const EvidenceSubject symptom = cotest::link_subject("symptom");
  const EvidenceSubject upstream = cotest::link_subject("upstream");

  CausalGraph graph;
  CausalEdge edge;
  edge.from = upstream;
  edge.to = symptom;
  edge.kind = CausalEdgeKind::kDemandUpstream;
  edge.rule_id = "observed-demand-rise";
  edge.citations.push_back(EvidenceId::from_digest({11, 12}));
  edge.strength = 20;
  edge.generation = generation;
  CO_EXPECT(graph.add_edge(edge, limits).ok());

  cotest::Source source = cotest::make_source("collector-1", generation);
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, symptom, 0.01, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, upstream, 0.95, cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kOfferedDemand, upstream, 130000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, upstream, 100000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));

  auto symptom_assessment = classify_subject(request_for(symptom, generation),
                                             cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(symptom_assessment);
  CO_EXPECT(symptom_assessment.value().congestion_asserted);

  LocalizationRequest request;
  request.symptom = symptom;
  request.window_start = cotest::at(-60);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;
  request.assessment = &symptom_assessment.value();

  auto result = localize(request, graph, cotest::pointers_to(records), ClassificationPolicy{},
                         LocalizationPolicy{}, limits);
  CO_EXPECT_OK(result);
  CO_EXPECT_EQ(result.value().outcome, LocalizationOutcome::kLocalized);
  CO_EXPECT_EQ(result.value().candidates.size(), static_cast<std::size_t>(1));
  if (!result.value().candidates.empty()) {
    CO_EXPECT_EQ(result.value().candidates.front().subject, upstream);
    CO_EXPECT(result.value().candidates.front().score >= 10);
    CO_EXPECT(!result.value().candidates.front().citations.empty());
    CO_EXPECT(!result.value().candidates.front().basis.empty());
  }
  CO_EXPECT_EQ(result.value().paths_examined, static_cast<std::size_t>(1));
}

CO_TEST(localize_episode, indistinguishable_candidates_are_reported_as_ambiguous) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  const EvidenceSubject symptom = cotest::link_subject("symptom");
  const EvidenceSubject first = cotest::link_subject("upstream-a");
  const EvidenceSubject second = cotest::link_subject("upstream-b");

  CausalGraph graph;
  for (const EvidenceSubject& candidate : {first, second}) {
    CausalEdge edge;
    edge.from = candidate;
    edge.to = symptom;
    edge.kind = CausalEdgeKind::kSharedContention;
    edge.rule_id = "shared-contention";
    edge.citations.push_back(EvidenceId::from_digest({21, 22}));
    edge.generation = generation;
    CO_EXPECT(graph.add_edge(edge, limits).ok());
  }

  cotest::Source source = cotest::make_source("collector-1", generation);
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, symptom, 0.01, cotest::at(0)));
  for (const EvidenceSubject& candidate : {first, second}) {
    records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, candidate, 0.80, cotest::at(0)));
    records.push_back(source.record(EvidenceKind::kActiveFlowCount, candidate, 8.0,
                                    ObservationUnit::kCount, ValueSemantics::kGauge,
                                    cotest::at(0)));
  }

  auto symptom_assessment = classify_subject(request_for(symptom, generation),
                                             cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(symptom_assessment);

  LocalizationRequest request;
  request.symptom = symptom;
  request.window_start = cotest::at(-60);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;
  request.assessment = &symptom_assessment.value();

  auto result = localize(request, graph, cotest::pointers_to(records), ClassificationPolicy{},
                         LocalizationPolicy{}, limits);
  CO_EXPECT_OK(result);
  CO_EXPECT_EQ(result.value().outcome, LocalizationOutcome::kAmbiguous);
  CO_EXPECT_EQ(result.value().candidates.size(), static_cast<std::size_t>(2));
  CO_EXPECT(result.value().blockers.front().code == localization_blockers::kAmbiguousCandidates);
  if (result.value().candidates.size() == 2) {
    CO_EXPECT(result.value().candidates[0].subject < result.value().candidates[1].subject);
  }

  // The same evaluation performed twice must be identical.
  auto repeat = localize(request, graph, cotest::pointers_to(records), ClassificationPolicy{},
                         LocalizationPolicy{}, limits);
  CO_EXPECT_OK(repeat);
  CO_EXPECT_EQ(repeat.value().outcome, result.value().outcome);
  CO_EXPECT_EQ(repeat.value().candidates.size(), result.value().candidates.size());
  if (!repeat.value().candidates.empty() && !result.value().candidates.empty()) {
    CO_EXPECT_EQ(repeat.value().candidates.front().score, result.value().candidates.front().score);
    CO_EXPECT_EQ(repeat.value().candidates.front().subject, result.value().candidates.front().subject);
  }
}

CO_TEST(localize_episode, search_depth_is_bounded_and_reported) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  const EvidenceSubject symptom = cotest::link_subject("n0");

  CausalGraph graph;
  for (int i = 0; i < 6; ++i) {
    CausalEdge edge;
    edge.from = cotest::link_subject(("n" + std::to_string(i + 1)).c_str());
    edge.to = cotest::link_subject(("n" + std::to_string(i)).c_str());
    edge.kind = CausalEdgeKind::kDemandUpstream;
    edge.rule_id = "chain";
    edge.citations.push_back(EvidenceId::from_digest({31, static_cast<std::uint64_t>(i)}));
    edge.generation = generation;
    CO_EXPECT(graph.add_edge(edge, limits).ok());
  }

  cotest::Source source = cotest::make_source("collector-1", generation);
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, symptom, 0.01, cotest::at(0)));
  for (int i = 1; i <= 6; ++i) {
    records.push_back(source.ratio(EvidenceKind::kQueueOccupancy,
                                   cotest::link_subject(("n" + std::to_string(i)).c_str()), 0.9,
                                   cotest::at(0)));
  }
  auto symptom_assessment = classify_subject(request_for(symptom, generation),
                                             cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(symptom_assessment);

  LocalizationRequest request;
  request.symptom = symptom;
  request.window_start = cotest::at(-60);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;
  request.assessment = &symptom_assessment.value();

  LocalizationPolicy policy;
  policy.max_depth = 2;
  auto result = localize(request, graph, cotest::pointers_to(records), ClassificationPolicy{}, policy,
                         limits);
  CO_EXPECT_OK(result);
  CO_EXPECT(result.value().depth_limited);
  for (const auto& candidate : result.value().candidates) {
    CO_EXPECT(candidate.depth <= 2);
  }
}

CO_TEST(localize_episode, episode_identity_is_deterministic_and_generation_scoped) {
  EpisodeKey key;
  key.scope = cotest::link_subject();
  key.mechanism = Mechanism::kPacketDrop;
  key.generation = cotest::test_generation();
  key.policy_version = "co-policy-1";
  const EpisodeId first = compute_episode_id(key);
  CO_EXPECT_EQ(first, compute_episode_id(key));
  CO_EXPECT(first.valid());

  EpisodeKey other = key;
  other.generation = cotest::test_generation(1, 2, 1);
  CO_EXPECT_NE(compute_episode_id(other), first);

  other = key;
  other.mechanism = Mechanism::kEcnMarking;
  CO_EXPECT_NE(compute_episode_id(other), first);

  other = key;
  other.policy_version = "co-policy-2";
  CO_EXPECT_NE(compute_episode_id(other), first);

  other = key;
  other.tenant = TenantId::unchecked("tenant-a");
  CO_EXPECT_NE(compute_episode_id(other), first);

  other = key;
  other.scope = cotest::link_subject("other");
  CO_EXPECT_NE(compute_episode_id(other), first);
}

CO_TEST(localize_episode, episode_lifecycle_is_monotonic) {
  const Limits limits = cotest::test_limits();
  EpisodeRegistry registry(limits);
  EpisodePolicy policy;
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  ClassificationRequest request = request_for(link, generation);
  std::vector<EvidenceRecord> records;
  // Just above the policy threshold: congestion is asserted at moderate severity.
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 2e-6, cotest::at(0)));

  auto moderate = classify_subject(request, cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(moderate);
  CO_EXPECT(moderate.value().congestion_asserted);

  auto opened = registry.observe(moderate.value(), policy, cotest::at(1));
  CO_EXPECT_OK(opened);
  CO_EXPECT_EQ(opened.value().kind, EpisodeUpdateKind::kOpened);
  CO_EXPECT(opened.value().created);
  CO_EXPECT_EQ(registry.size(), static_cast<std::size_t>(1));

  // Re-observing the same classification updates rather than duplicating.
  auto updated = registry.observe(moderate.value(), policy, cotest::at(2));
  CO_EXPECT_OK(updated);
  CO_EXPECT_EQ(updated.value().id, opened.value().id);
  CO_EXPECT_EQ(registry.size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(updated.value().revision.value(), static_cast<std::uint64_t>(2));

  // A more severe observation escalates.
  std::vector<EvidenceRecord> severe_records;
  severe_records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(2)));
  severe_records.push_back(source.ratio(EvidenceKind::kMarkCount, link, 0.4, cotest::at(2)));
  auto severe_request = request_for(link, generation, 3);
  auto severe = classify_subject(severe_request, cotest::pointers_to(severe_records),
                                 ClassificationPolicy{});
  CO_EXPECT_OK(severe);
  auto escalated = registry.observe(severe.value(), policy, cotest::at(3));
  CO_EXPECT_OK(escalated);
  CO_EXPECT_EQ(escalated.value().kind, EpisodeUpdateKind::kEscalated);
  CO_EXPECT_EQ(escalated.value().id, opened.value().id);

  auto episode = registry.get(opened.value().id);
  CO_EXPECT_OK(episode);
  CO_EXPECT_EQ(episode.value().peak_severity, Severity::kCritical);
  CO_EXPECT_EQ(episode.value().observations, static_cast<std::uint64_t>(3));
  CO_EXPECT(episode.value().transitions.size() >= 3);
  CO_EXPECT_EQ(episode.value().transitions.front().kind, EpisodeTransitionKind::kOpened);

  // A quiescence window moves the episode to quiescent, then resolved, then expired.
  auto advanced = registry.advance(cotest::at(3 + 31), policy, CancellationToken{});
  CO_EXPECT_OK(advanced);
  CO_EXPECT_EQ(advanced.value().size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(advanced.value().front().kind, EpisodeUpdateKind::kQuiesced);

  advanced = registry.advance(cotest::at(3 + 61), policy, CancellationToken{});
  CO_EXPECT_OK(advanced);
  CO_EXPECT_EQ(advanced.value().front().kind, EpisodeUpdateKind::kResolved);

  advanced = registry.advance(cotest::at(3 + 600 + 60), policy, CancellationToken{});
  CO_EXPECT_OK(advanced);
  CO_EXPECT_EQ(advanced.value().front().kind, EpisodeUpdateKind::kExpired);

  advanced = registry.advance(cotest::at(3 + 100000), policy, CancellationToken{});
  CO_EXPECT_OK(advanced);
  CO_EXPECT(advanced.value().empty());

  // Fresh evidence reopens an expired episode with the same identity.
  auto reopened = registry.observe(severe.value(), policy, cotest::at(3 + 100001));
  CO_EXPECT_OK(reopened);
  CO_EXPECT_EQ(reopened.value().id, opened.value().id);
  CO_EXPECT_EQ(reopened.value().kind, EpisodeUpdateKind::kOpened);
  auto reopened_episode = registry.get(opened.value().id);
  CO_EXPECT_OK(reopened_episode);
  CO_EXPECT_EQ(reopened_episode.value().state, EpisodeState::kOpen);
}

CO_TEST(localize_episode, only_congestion_assertions_create_history) {
  const Limits limits = cotest::test_limits();
  EpisodeRegistry registry(limits);
  EpisodePolicy policy;
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> utilization_only;
  utilization_only.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  auto healthy = classify_subject(request_for(link, generation), cotest::pointers_to(utilization_only),
                                  ClassificationPolicy{});
  CO_EXPECT_OK(healthy);
  CO_EXPECT_EQ(healthy.value().verdict, Verdict::kUtilizedHealthy);

  auto outcome = registry.observe(healthy.value(), policy, cotest::at(1));
  CO_EXPECT_OK(outcome);
  CO_EXPECT_EQ(outcome.value().kind, EpisodeUpdateKind::kUnchanged);
  CO_EXPECT_EQ(registry.size(), static_cast<std::size_t>(0));
}

CO_TEST(localize_episode, episode_history_is_bounded_and_reports_truncation) {
  Limits limits = cotest::test_limits();
  limits.max_episode_transitions = 4;
  limits.max_episode_assessments = 3;
  CO_EXPECT(limits.validate().ok());
  EpisodeRegistry registry(limits);
  EpisodePolicy policy;
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.01, cotest::at(0)));
  auto assessment = classify_subject(request_for(link, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);

  EpisodeId id;
  for (int i = 0; i < 12; ++i) {
    auto update = registry.observe(assessment.value(), policy, cotest::at(1 + i));
    CO_EXPECT_OK(update);
    id = update.value().id;
  }
  auto episode = registry.get(id);
  CO_EXPECT_OK(episode);
  CO_EXPECT_EQ(episode.value().transitions.size(), static_cast<std::size_t>(4));
  CO_EXPECT_EQ(episode.value().assessments.size(), static_cast<std::size_t>(3));
  CO_EXPECT(episode.value().history_truncated);
  CO_EXPECT_EQ(episode.value().transitions_dropped, static_cast<std::uint64_t>(8));
  CO_EXPECT_EQ(episode.value().observations, static_cast<std::uint64_t>(12));
}

CO_TEST(localize_episode, episode_registry_enforces_its_budget) {
  Limits limits = cotest::test_limits();
  limits.max_episodes = 2;
  limits.max_group_members = 2;
  limits.max_snapshot_episodes = 2;
  CO_EXPECT(limits.validate().ok());
  EpisodeRegistry registry(limits);
  EpisodePolicy policy;
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);

  for (int i = 0; i < 3; ++i) {
    const EvidenceSubject subject = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    std::vector<EvidenceRecord> records;
    records.push_back(source.ratio(EvidenceKind::kDropCount, subject, 0.01, cotest::at(0)));
    auto assessment = classify_subject(request_for(subject, generation),
                                       cotest::pointers_to(records), ClassificationPolicy{});
    CO_EXPECT_OK(assessment);
    auto update = registry.observe(assessment.value(), policy, cotest::at(1));
    if (i < 2) {
      CO_EXPECT_OK(update);
    } else {
      // No terminal episode exists yet, so the third one cannot displace an active investigation.
      CO_EXPECT_ERR(update, ErrorCode::kLimitExceeded);
      CO_EXPECT_EQ(registry.dropped_episodes(), static_cast<std::uint64_t>(1));
    }
  }
  CO_EXPECT(registry.size() <= 2);
}

CO_TEST(localize_episode, correlation_groups_related_episodes_deterministically) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  EpisodeRegistry registry(limits);
  EpisodePolicy episode_policy;
  cotest::Source source = cotest::make_source("collector-1", generation);

  std::vector<Episode> episodes;
  for (int i = 0; i < 3; ++i) {
    const EvidenceSubject subject = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    std::vector<EvidenceRecord> records;
    records.push_back(source.ratio(EvidenceKind::kDropCount, subject, 0.01, cotest::at(i * 2)));
    auto assessment = classify_subject(request_for(subject, generation, 1 + i * 2),
                                       cotest::pointers_to(records), ClassificationPolicy{});
    CO_EXPECT_OK(assessment);
    CO_EXPECT(registry.observe(assessment.value(), episode_policy, cotest::at(1 + i * 2)).ok());
  }
  episodes = registry.all();
  CO_EXPECT_EQ(episodes.size(), static_cast<std::size_t>(3));

  CorrelationPolicy policy;
  auto groups = correlate(episodes, policy, limits, CancellationToken{});
  CO_EXPECT_OK(groups);
  CO_EXPECT_EQ(groups.value().size(), static_cast<std::size_t>(1));
  if (!groups.value().empty()) {
    CO_EXPECT_EQ(groups.value().front().members.size(), static_cast<std::size_t>(3));
    CO_EXPECT(std::is_sorted(groups.value().front().members.begin(),
                             groups.value().front().members.end()));
    CO_EXPECT(!groups.value().front().decisions.empty());
  }

  const auto repeat = correlate(episodes, policy, limits, CancellationToken{});
  CO_EXPECT_OK(repeat);
  CO_EXPECT_EQ(repeat.value().front().id, groups.value().front().id);
  CO_EXPECT_EQ(compute_group_id(groups.value().front().members), groups.value().front().id);
}

CO_TEST(localize_episode, correlation_refuses_to_over_merge) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  EpisodeRegistry registry(limits);
  EpisodePolicy episode_policy;
  cotest::Source drop_source = cotest::make_source("collector-1", generation);
  cotest::Source mark_source = cotest::make_source("collector-2", generation);

  const EvidenceSubject first = cotest::link_subject("link-a");
  const EvidenceSubject second = cotest::link_subject("link-b");

  std::vector<EvidenceRecord> drop_records;
  drop_records.push_back(drop_source.ratio(EvidenceKind::kDropCount, first, 0.01, cotest::at(0)));
  auto drop_assessment = classify_subject(request_for(first, generation),
                                          cotest::pointers_to(drop_records),
                                          ClassificationPolicy{});
  CO_EXPECT_OK(drop_assessment);
  CO_EXPECT(registry.observe(drop_assessment.value(), episode_policy, cotest::at(1)).ok());

  std::vector<EvidenceRecord> mark_records;
  mark_records.push_back(mark_source.ratio(EvidenceKind::kMarkCount, second, 0.5, cotest::at(0)));
  auto mark_assessment = classify_subject(request_for(second, generation),
                                          cotest::pointers_to(mark_records),
                                          ClassificationPolicy{});
  CO_EXPECT_OK(mark_assessment);
  CO_EXPECT(registry.observe(mark_assessment.value(), episode_policy, cotest::at(1)).ok());

  const auto episodes = registry.all();
  CorrelationPolicy policy;
  auto groups = correlate(episodes, policy, limits, CancellationToken{});
  CO_EXPECT_OK(groups);
  CO_EXPECT_EQ(groups.value().size(), static_cast<std::size_t>(2));
  for (const auto& group : groups.value()) {
    CO_EXPECT_EQ(group.members.size(), static_cast<std::size_t>(1));
  }
  bool saw_mechanism_rejection = false;
  for (const auto& group : groups.value()) {
    for (const auto& decision : group.decisions) {
      if (decision.code == MergeDecisionCode::kRejectedMechanismMismatch) {
        saw_mechanism_rejection = true;
      }
    }
  }
  CO_EXPECT(saw_mechanism_rejection);

  // Merging the same mechanism across a wide temporal gap is also refused.
  CorrelationPolicy narrow;
  narrow.max_gap = Duration::from_seconds(1);
  std::vector<Episode> spread;
  Episode far = episodes.front();
  far.last_seen = Timestamp(far.last_seen.unix_nanos() + 3600LL * 1000000000LL);
  far.first_seen = far.last_seen;
  spread.push_back(episodes.front());
  spread.push_back(far);
  auto separated = correlate(spread, narrow, limits, CancellationToken{});
  CO_EXPECT_OK(separated);
  CO_EXPECT_EQ(separated.value().size(), static_cast<std::size_t>(2));

  CorrelationPolicy capped;
  capped.max_group_members = 2;
  auto capped_groups = correlate(episodes, capped, limits, CancellationToken{});
  CO_EXPECT_OK(capped_groups);
  std::size_t total_members = 0;
  for (const auto& group : capped_groups.value()) {
    total_members += group.members.size();
    CO_EXPECT(group.members.size() <= 2);
  }
  CO_EXPECT_EQ(total_members, static_cast<std::size_t>(2));
}

CO_TEST(localize_episode, correlation_is_cancellable) {
  const Limits limits = cotest::test_limits();
  std::vector<Episode> episodes;
  for (int i = 0; i < 4; ++i) {
    Episode episode;
    EpisodeKey key;
    key.scope = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    key.mechanism = Mechanism::kPacketDrop;
    key.generation = cotest::test_generation();
    key.policy_version = "co-policy-1";
    episode.key = key;
    episode.id = compute_episode_id(key);
    episode.first_seen = cotest::at(i);
    episode.last_seen = cotest::at(i);
    episodes.push_back(episode);
  }
  CancellationSource source;
  source.cancel("test");
  CO_EXPECT_ERR(correlate(episodes, CorrelationPolicy{}, limits, source.token()),
                ErrorCode::kCancelled);
}

CO_TEST(localize_episode, episode_restore_preserves_revision_and_history) {
  const Limits limits = cotest::test_limits();
  EpisodeRegistry registry(limits);
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.01, cotest::at(0)));
  auto assessment = classify_subject(request_for(link, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  auto update = registry.observe(assessment.value(), EpisodePolicy{}, cotest::at(1));
  CO_EXPECT_OK(update);
  auto original = registry.get(update.value().id);
  CO_EXPECT_OK(original);

  EpisodeRegistry restored(limits);
  CO_EXPECT(restored.restore(original.value()).ok());
  auto reloaded = restored.get(update.value().id);
  CO_EXPECT_OK(reloaded);
  CO_EXPECT_EQ(reloaded.value().revision.value(), original.value().revision.value());
  CO_EXPECT_EQ(reloaded.value().transitions.size(), original.value().transitions.size());
  CO_EXPECT_EQ(reloaded.value().observations, original.value().observations);
  CO_EXPECT_EQ(reloaded.value().id, original.value().id);

  // Restoring an older revision over a newer one is a conflict, not a silent downgrade.
  Episode older = original.value();
  older.revision = Revision(0);
  CO_EXPECT_STATUS_ERR(restored.restore(older), ErrorCode::kConflict);

  Episode invalid;
  CO_EXPECT_STATUS_ERR(restored.restore(invalid), ErrorCode::kInvalidArgument);
}
