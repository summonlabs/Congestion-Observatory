// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <thread>
#include <vector>

using namespace congestion;

namespace {

EvidenceRecord topology_advertisement(cotest::Source& source, const EvidenceSubject& subject) {
  return source.record(EvidenceKind::kTopologyAdvertisement, subject, 1.0,
                       ObservationUnit::kBoolean, ValueSemantics::kGauge, cotest::at(0));
}

}  // namespace

CO_TEST(runtime, ingest_classify_localize_end_to_end) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  const auto generation = topology.generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject symptom = cotest::link_subject("leaf1-leaf2");
  const EvidenceSubject upstream = cotest::link_subject("host1-leaf1");

  // Structural edges only exist where the topology was actually advertised.
  for (const char* link : {"host1-leaf1", "leaf1-leaf2", "leaf2-host2"}) {
    auto record = topology_advertisement(source, cotest::link_subject(link));
    CO_EXPECT(runtime->ingest(record, cotest::at(0)).value().stored);
  }
  CO_EXPECT(runtime->rebuild_causal_graph().ok());
  CO_EXPECT(runtime->causal_graph().edge_count() > 0);

  std::vector<EvidenceRecord> impairment;
  impairment.push_back(source.ratio(EvidenceKind::kDropCount, symptom, 0.02, cotest::at(1)));
  impairment.push_back(source.ratio(EvidenceKind::kLinkUtilization, symptom, 0.99, cotest::at(1)));
  for (const EvidenceRecord& record : impairment) {
    auto outcome = runtime->ingest(record, cotest::at(1));
    CO_EXPECT_OK(outcome);
    CO_EXPECT(outcome.value().stored);
  }

  // Upstream evidence that explains the symptom.
  std::vector<EvidenceRecord> upstream_records;
  upstream_records.push_back(source.record(EvidenceKind::kOfferedDemand, upstream, 30000000000.0,
                                           ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                           cotest::at(1)));
  upstream_records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, upstream,
                                           25000000000.0, ObservationUnit::kBitsPerSecond,
                                           ValueSemantics::kGauge, cotest::at(1)));
  upstream_records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, upstream, 0.95,
                                          cotest::at(1)));
  for (const EvidenceRecord& record : upstream_records) {
    CO_EXPECT(runtime->ingest(record, cotest::at(1)).value().stored);
  }

  auto assessment = runtime->classify(symptom, Duration::from_seconds(30), cotest::at(2));
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kCongestionConfirmed);
  CO_EXPECT(assessment.value().congestion_asserted);

  auto localization = runtime->localize(symptom, Duration::from_seconds(30), cotest::at(2));
  CO_EXPECT_OK(localization);
  CO_EXPECT(localization.value().outcome == LocalizationOutcome::kLocalized ||
            localization.value().outcome == LocalizationOutcome::kAmbiguous);

  EpisodeUpdate update;
  auto evaluated = runtime->evaluate(symptom, Duration::from_seconds(30), cotest::at(2), &update);
  CO_EXPECT_OK(evaluated);
  CO_EXPECT_EQ(update.kind, EpisodeUpdateKind::kOpened);
  CO_EXPECT_EQ(runtime->episodes(EpisodeFilter{}).size(), static_cast<std::size_t>(1));

  auto groups = runtime->correlate(cotest::at(2));
  CO_EXPECT_OK(groups);
  CO_EXPECT_EQ(groups.value().size(), static_cast<std::size_t>(1));

  auto explanation = runtime->explain(symptom, Duration::from_seconds(30), cotest::at(2));
  CO_EXPECT_OK(explanation);
  CO_EXPECT(!explanation.value().steps.empty());
  CO_EXPECT(explanation.value().render().find("R04-impairment") != std::string::npos);

  auto episode_explanation = runtime->explain_episode(update.id);
  CO_EXPECT_OK(episode_explanation);

  ExportRequest request;
  request.include_evidence = true;
  auto exported = runtime->export_json(request, cotest::at(2));
  CO_EXPECT_OK(exported);
  auto document = parse_json(exported.value(), cotest::test_limits());
  CO_EXPECT_OK(document);
  if (document.ok()) {
    CO_EXPECT(document.value().member("episodes") != nullptr);
    CO_EXPECT_EQ(document.value().member("episodes")->as_array().size(),
                 static_cast<std::size_t>(1));
    CO_EXPECT(document.value().member("metrics") != nullptr);
  }

  const RuntimeMetrics metrics = runtime->metrics();
  CO_EXPECT(metrics.ingested >= 6);
  // classify, the classification inside localize, evaluate and explain.
  CO_EXPECT_EQ(metrics.classifications, static_cast<std::uint64_t>(4));
  CO_EXPECT_EQ(metrics.episodes_opened, static_cast<std::uint64_t>(1));
  CO_EXPECT_EQ(metrics.evidence_retained, static_cast<std::size_t>(metrics.ingested));
  CO_EXPECT_EQ(metrics.live_sources, static_cast<std::size_t>(1));
}

CO_TEST(runtime, unknown_subjects_are_refused) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  CO_EXPECT_ERR(runtime->classify(cotest::link_subject("missing"), Duration::from_seconds(10),
                                  cotest::at(0)),
                ErrorCode::kNotFound);
  CO_EXPECT_ERR(runtime->classify(cotest::link_subject(), Duration::zero(), cotest::at(0)),
                ErrorCode::kInvalidArgument);

  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  EvidenceRecord unknown = source.ratio(EvidenceKind::kLinkUtilization,
                                        cotest::link_subject("missing"), 0.5, cotest::at(0));
  CO_EXPECT_ERR(runtime->ingest(unknown, cotest::at(0)), ErrorCode::kNotFound);
  CO_EXPECT_EQ(runtime->metrics().ingest_rejected_unknown_subject, static_cast<std::uint64_t>(1));
}

CO_TEST(runtime, runtime_configuration_is_validated) {
  RuntimeConfig config;
  config.classification.version = "";
  CO_EXPECT_ERR(Runtime::create(config), ErrorCode::kInvalidArgument);

  RuntimeConfig limits_config;
  limits_config.limits.max_sources = 0;
  CO_EXPECT_ERR(Runtime::create(limits_config), ErrorCode::kInvalidArgument);

  RuntimeConfig policy_config;
  policy_config.classification.utilization_attention = 0.9;
  policy_config.classification.utilization_high = 0.5;
  CO_EXPECT_ERR(Runtime::create(policy_config), ErrorCode::kInvalidArgument);

  RuntimeConfig depth_config;
  depth_config.localization.max_depth = depth_config.limits.max_localization_depth + 1;
  CO_EXPECT_ERR(Runtime::create(depth_config), ErrorCode::kInvalidArgument);

  RuntimeConfig worker_config;
  worker_config.limits.worker_threads = 1;
  worker_config.limits.max_pending_tasks = 0;
  auto rejected = Runtime::create(worker_config);
  CO_EXPECT_ERR(rejected, ErrorCode::kInvalidArgument);
}

CO_TEST(runtime, shutdown_is_real_and_observable) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                       cotest::at(0));
  CO_EXPECT(runtime->ingest(record, cotest::at(0)).ok());

  CO_EXPECT(!runtime->shutting_down());
  runtime->request_shutdown();
  CO_EXPECT(runtime->shutting_down());
  CO_EXPECT(runtime->cancellation_token().cancelled());
  CO_EXPECT_ERR(runtime->ingest(record, cotest::at(1)), ErrorCode::kShuttingDown);
  CO_EXPECT(runtime->metrics().shutting_down);
}

CO_TEST(runtime, worker_pool_requires_configuration_and_runs_bounded_work) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  RuntimeConfig config = cotest::make_runtime_config(topology, cotest::test_limits());
  auto synchronous = Runtime::create(config);
  CO_EXPECT_OK(synchronous);
  CO_EXPECT_STATUS_ERR(synchronous.value()->start_workers(), ErrorCode::kUnsupported);
  CO_EXPECT_STATUS_ERR(synchronous.value()->request_maintenance(), ErrorCode::kUnsupported);

  RuntimeConfig threaded = cotest::make_runtime_config(topology, cotest::test_limits());
  threaded.limits.worker_threads = 2;
  threaded.limits.max_pending_tasks = 4;
  auto runtime = Runtime::create(threaded);
  CO_EXPECT_OK(runtime);
  if (!runtime.ok()) {
    return;
  }
  CO_EXPECT(runtime.value()->start_workers().ok());
  CO_EXPECT(runtime.value()->metrics().workers_running);
  for (int i = 0; i < 4; ++i) {
    CO_EXPECT(runtime.value()->request_maintenance().ok());
  }
  CO_EXPECT(runtime.value()->stop_workers().ok());
  CO_EXPECT(!runtime.value()->metrics().workers_running);

  // After shutdown the pool refuses new work instead of blocking.
  std::size_t refused = 0;
  for (int i = 0; i < 8; ++i) {
    if (!runtime.value()->request_maintenance().ok()) {
      ++refused;
    }
  }
  CO_EXPECT_EQ(refused, static_cast<std::size_t>(8));
  CO_EXPECT(runtime.value()->metrics().tasks_rejected > 0);
}

CO_TEST(runtime, maintenance_advances_episodes_and_sweeps_evidence) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  const EvidenceSubject link = cotest::link_subject();
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  for (const EvidenceRecord& record : records) {
    CO_EXPECT(runtime->ingest(record, cotest::at(0)).value().stored);
  }
  EpisodeUpdate update;
  CO_EXPECT(runtime->evaluate(link, Duration::from_seconds(10), cotest::at(1), &update).ok());
  CO_EXPECT_EQ(update.kind, EpisodeUpdateKind::kOpened);

  const std::size_t before = runtime->metrics().evidence_retained;
  CO_EXPECT(before >= 2);
  auto changes = runtime->run_maintenance(cotest::at(1 + 31));
  CO_EXPECT_OK(changes);
  const auto episodes = runtime->episodes(EpisodeFilter{});
  CO_EXPECT_EQ(episodes.size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(episodes.front().state, EpisodeState::kQuiescent);

  // Far in the future the retention horizon removes the ingested evidence.
  auto later = runtime->run_maintenance(cotest::at(1 + 100000));
  CO_EXPECT_OK(later);
  CO_EXPECT(runtime->metrics().evidence_retained < before);
}

CO_TEST(runtime, topology_admission_replaces_the_generation) {
  const Topology first = cotest::make_test_topology(cotest::test_generation(1, 1, 1));
  auto runtime = cotest::make_runtime(first, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  CO_EXPECT_EQ(runtime->topology().generation().str(), std::string("1/1/1"));
  const Topology second = cotest::make_test_topology(cotest::test_generation(1, 2, 1));
  CO_EXPECT(runtime->admit_topology(second).ok());
  CO_EXPECT_EQ(runtime->topology().generation().str(), std::string("1/2/1"));

  // Evidence from the replaced generation no longer contributes to the new one.
  cotest::Source old_source = cotest::make_source("collector-old", cotest::test_generation(1, 1, 1));
  CO_EXPECT(runtime
                ->ingest(old_source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5,
                                          cotest::at(0)),
                         cotest::at(0))
                .value()
                .stored);
  auto assessment = runtime->classify(cotest::link_subject(), Duration::from_seconds(10),
                                      cotest::at(1));
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kIndeterminate);
  CO_EXPECT(assessment.value().has_blocker(blockers::kGenerationMismatch));
}

CO_TEST(runtime, causal_edges_can_be_added_explicitly_and_require_evidence) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  CausalEdge edge;
  edge.from = cotest::link_subject("host1-leaf1");
  edge.to = cotest::link_subject("leaf1-leaf2");
  edge.kind = CausalEdgeKind::kDemandUpstream;
  edge.rule_id = "operator-hypothesis";
  CO_EXPECT_STATUS_ERR(runtime->add_causal_edge(edge), ErrorCode::kPreconditionFailed);
  edge.citations.push_back(EvidenceId::from_digest({5, 5}));
  CO_EXPECT(runtime->add_causal_edge(edge).ok());
  CO_EXPECT_EQ(runtime->causal_graph().edge_count(), static_cast<std::size_t>(1));
}

CO_TEST(runtime, structural_edges_need_a_topology_observation) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  // A topology alone is a claim: without an advertisement record no structural edge is created.
  CO_EXPECT(runtime->rebuild_causal_graph().ok());
  CO_EXPECT_EQ(runtime->causal_graph().edge_count(), static_cast<std::size_t>(0));

  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  for (const char* link : {"host1-leaf1", "leaf1-leaf2", "leaf2-host2"}) {
    CO_EXPECT(runtime->ingest(topology_advertisement(source, cotest::link_subject(link)),
                              cotest::at(0))
                  .value()
                  .stored);
  }
  CO_EXPECT(runtime->rebuild_causal_graph().ok());
  CO_EXPECT(runtime->causal_graph().edge_count() > 0);
}
