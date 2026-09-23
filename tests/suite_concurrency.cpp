// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Concurrency: ownership, bounded pools, real cancellation.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace congestion;

CO_TEST(concurrency, parallel_ingest_from_independent_sources_is_lossless) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  constexpr int kThreads = 4;
  constexpr int kRecordsPerThread = 50;
  std::atomic<int> accepted{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      cotest::Source source =
          cotest::make_source("collector-" + std::to_string(t), topology.generation());
      for (int i = 0; i < kRecordsPerThread; ++i) {
        EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization,
                                             cotest::link_subject(),
                                             0.5 + 0.001 * static_cast<double>(i), cotest::at(0));
        auto outcome = runtime->ingest(record, cotest::at(0));
        if (!outcome.ok()) {
          failed.fetch_add(1);
          continue;
        }
        if (outcome.value().stored) {
          accepted.fetch_add(1);
        } else {
          failed.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CO_EXPECT_EQ(failed.load(), 0);
  CO_EXPECT_EQ(accepted.load(), kThreads * kRecordsPerThread);
  CO_EXPECT_EQ(runtime->metrics().evidence_retained,
               static_cast<std::size_t>(kThreads * kRecordsPerThread));
  CO_EXPECT_EQ(runtime->metrics().live_sources, static_cast<std::size_t>(kThreads));
}

CO_TEST(concurrency, parallel_analysis_while_ingesting_is_consistent) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  std::atomic<bool> stop{false};
  std::atomic<int> analysis_errors{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        auto assessment = runtime->classify(cotest::link_subject(), Duration::from_seconds(60),
                                            cotest::at(1));
        if (!assessment.ok()) {
          analysis_errors.fetch_add(1);
        } else if (assessment.value().confidence > 100) {
          analysis_errors.fetch_add(1);
        }
        auto explanation = runtime->explain(cotest::link_subject(), Duration::from_seconds(60),
                                            cotest::at(1));
        if (!explanation.ok()) {
          analysis_errors.fetch_add(1);
        }
        auto exported = runtime->export_json(ExportRequest{}, cotest::at(1));
        if (!exported.ok()) {
          analysis_errors.fetch_add(1);
        }
        (void)runtime->metrics();
        (void)runtime->episodes(EpisodeFilter{});
      }
    });
  }
  std::vector<std::thread> writers;
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&, w]() {
      cotest::Source source =
          cotest::make_source("writer-" + std::to_string(w), topology.generation());
      for (int i = 0; i < 100; ++i) {
        EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.001,
                                             cotest::at(0));
        auto outcome = runtime->ingest(record, cotest::at(0));
        if (!outcome.ok()) {
          analysis_errors.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : writers) {
    thread.join();
  }
  stop.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }
  CO_EXPECT_EQ(analysis_errors.load(), 0);
  CO_EXPECT_EQ(runtime->metrics().evidence_retained, static_cast<std::size_t>(200));
}

CO_TEST(concurrency, worker_pool_drains_every_task_on_shutdown) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  RuntimeConfig config = cotest::make_runtime_config(topology, cotest::test_limits());
  config.limits.worker_threads = 3;
  config.limits.max_pending_tasks = 64;
  auto created = Runtime::create(config);
  CO_EXPECT_OK(created);
  if (!created.ok()) {
    return;
  }
  auto& runtime = *created.value();
  CO_EXPECT(runtime.start_workers().ok());

  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  CO_EXPECT(runtime
                .ingest(source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5,
                                     cotest::at(0)),
                        cotest::at(0))
                .value()
                .stored);

  std::size_t submitted = 0;
  for (int i = 0; i < 32; ++i) {
    if (runtime.request_maintenance().ok()) {
      ++submitted;
    }
  }
  CO_EXPECT(submitted > 0);
  // Shutdown joins every worker; no task is abandoned and none is left running.
  CO_EXPECT(runtime.stop_workers().ok());
  const RuntimeMetrics metrics = runtime.metrics();
  CO_EXPECT_EQ(metrics.tasks_submitted, static_cast<std::uint64_t>(submitted));
  CO_EXPECT_EQ(metrics.tasks_completed, static_cast<std::uint64_t>(submitted));
  CO_EXPECT(!metrics.workers_running);
}

CO_TEST(concurrency, cancellation_stops_cooperative_work) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const auto generation = topology.generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;
  for (int i = 0; i < 40; ++i) {
    CO_EXPECT(store.ingest(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                        cotest::at(i)),
                           cotest::at(i), policy)
                  .value()
                  .stored);
  }
  // Case A: cancellation already requested. The sweep must refuse before doing any work.
  CancellationSource pre_cancelled;
  pre_cancelled.cancel("already shutting down");
  auto refused = store.sweep(cotest::at(1000), cotest::at(2000), 100, pre_cancelled.token());
  CO_REQUIRE(!refused.ok());
  CO_EXPECT_EQ(refused.error().code(), ErrorCode::kCancelled);
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(40));

  // Case B: cancellation requested concurrently with the sweep. The outcome is either a completed
  // sweep or a cancelled one, and the store stays bounded either way.
  CancellationSource cancellation;
  std::atomic<bool> cancel_observed{false};
  std::thread canceller([&]() {
    cancellation.cancel("concurrent shutdown");
    cancel_observed.store(true);
  });
  auto swept = store.sweep(cotest::at(1000), cotest::at(2000), 100, cancellation.token());
  canceller.join();
  CO_EXPECT(cancel_observed.load());
  if (!swept.ok()) {
    CO_EXPECT_EQ(swept.error().code(), ErrorCode::kCancelled);
  }
  CO_EXPECT(store.size() <= 40);
}

CO_TEST(concurrency, concurrent_readers_never_observe_partial_state) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  std::atomic<int> errors{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        // Each read is a consistent snapshot of one component. Two separate reads may legitimately
        // differ while a writer is running, so only within-view invariants are asserted.
        const RuntimeMetrics metrics = runtime->metrics();
        if (metrics.live_sources > metrics.known_sources) {
          errors.fetch_add(1);
        }
        if (metrics.evidence_evicted > 0 && metrics.evidence_retained == 0) {
          errors.fetch_add(1);
        }
        const StoreStats evidence = runtime->evidence().stats();
        if (evidence.live_sources > evidence.sources) {
          errors.fetch_add(1);
        }
        if (evidence.retained > runtime->limits().max_retained_evidence) {
          errors.fetch_add(1);
        }
      }
    });
  }
  for (int i = 0; i < 200; ++i) {
    CO_EXPECT(runtime->ingest(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                           0.5, cotest::at(0)),
                              cotest::at(0))
                  .ok());
  }
  stop.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }
  CO_EXPECT_EQ(errors.load(), 0);
  CO_EXPECT_EQ(runtime->metrics().evidence_retained, static_cast<std::size_t>(200));
}
