// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Deliberate attempts to break the runtime: hostile peers, reordered delivery, concurrency over
// the whole public surface, and file level races.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace congestion;

namespace {

std::string evidence_document(const std::vector<EvidenceRecord>& records) {
  std::string document = "[";
  bool first = true;
  for (const EvidenceRecord& record : records) {
    if (!first) {
      document += ",";
    }
    first = false;
    document += encode_ingest_json(record);
  }
  document += "]";
  return document;
}

}  // namespace

CO_TEST(hardening, hostile_peer_cannot_stall_the_server) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_REQUIRE(runtime != nullptr);

  ServerConfig config;
  config.socket.io_deadline = Duration::from_seconds(2);
  config.max_connections = 4;
  IngestServer server(*runtime, config);
  auto port = server.start();
  CO_REQUIRE_OK(port);

  CancellationSource cancellation;
  std::thread server_thread([&]() { (void)server.run(cancellation.token()); });

  // A peer that connects and sends nothing must not prevent a well behaved collector from working.
  SocketOptions options;
  options.io_deadline = Duration::from_seconds(5);
  auto silent = Socket::connect_tcp("127.0.0.1", port.value(), options);
  CO_REQUIRE_OK(silent);

  // Garbage that is not a frame at all.
  const std::string garbage = "this is not a frame at all, not even close";
  CO_EXPECT(silent.value().send_all(garbage).ok());

  IngestClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = port.value();
  client_config.socket.io_deadline = Duration::from_seconds(10);
  cotest::Source source = cotest::make_source("well-behaved", topology.generation());
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.01,
                                 cotest::at(0)));
  auto pushed = push_records(client_config, records);
  CO_REQUIRE_OK(pushed);
  CO_EXPECT_EQ(pushed.value().records_accepted, static_cast<std::uint64_t>(1));

  silent.value().close();
  server.stop();
  cancellation.cancel("test complete");
  server_thread.join();

  const ServerStats stats = server.stats();
  CO_EXPECT(stats.connections_accepted >= 2);
  CO_EXPECT(stats.protocol_errors >= 1);
  CO_EXPECT_EQ(runtime->metrics().ingested, static_cast<std::uint64_t>(1));
}

CO_TEST(hardening, reordered_delivery_is_fenced_and_never_lost) {
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  IngestPolicy policy;

  // Ten observations per source, delivered newest first: the fence must refuse the replays and
  // keep exactly one record per address.
  std::uint64_t attempts = 0;
  std::uint64_t accepted = 0;
  for (int index = 0; index < 3; ++index) {
    cotest::Source source =
        cotest::make_source("collector-" + std::to_string(index), generation);
    std::vector<EvidenceRecord> records;
    for (int i = 0; i < 10; ++i) {
      records.push_back(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                     0.1 * static_cast<double>(i), cotest::at(i)));
    }
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
      ++attempts;
      auto outcome = store.ingest(*it, cotest::at(0), policy);
      CO_REQUIRE_OK(outcome);
      if (outcome.value().stored) {
        ++accepted;
      }
    }
  }
  CO_EXPECT_EQ(attempts, static_cast<std::uint64_t>(30));
  CO_EXPECT_EQ(accepted, static_cast<std::uint64_t>(3));
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(3));
  CO_EXPECT_EQ(store.stats().accepted + store.stats().rejected_fence, attempts);

  // The retained record per source is the newest one the fence let through.
  EvidenceQuery query;
  query.max_records = 64;
  const auto retained = store.query(query);
  CO_EXPECT_EQ(retained.size(), static_cast<std::size_t>(3));
  for (const EvidenceRecord& record : retained) {
    CO_EXPECT_EQ(record.fence.sequence.value(), static_cast<std::uint64_t>(10));
  }
}

CO_TEST(hardening, incomparable_generation_is_refused_by_the_store) {
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  IngestPolicy policy;
  const auto generation = cotest::test_generation(1, 4, 0);

  cotest::Source source = cotest::make_source("collector-1", generation);
  EvidenceRecord first = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                      cotest::at(0));
  CO_REQUIRE_OK(store.ingest(first, cotest::at(0), policy));

  // Higher epoch but lower generation: not orderable, so it must be refused rather than guessed.
  cotest::Source inconsistent = cotest::make_source("collector-1", cotest::test_generation(2, 1, 0));
  inconsistent.boot = source.boot;
  inconsistent.incarnation = source.incarnation;
  inconsistent.sequence = source.sequence;
  EvidenceRecord record = inconsistent.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                             0.9, cotest::at(1));
  auto outcome = store.ingest(record, cotest::at(1), policy);
  CO_REQUIRE_OK(outcome);
  CO_EXPECT(!outcome.value().stored);
  CO_EXPECT_EQ(outcome.value().fence, FenceDecision::kRejectedIncomparableGeneration);
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(1));
}

CO_TEST(hardening, whole_public_surface_under_concurrency) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  RuntimeConfig config = cotest::make_runtime_config(topology, cotest::test_limits());
  config.state_directory = cotest::unique_temp_dir("hardening-concurrency");
  config.state_basename = "state";
  auto created = Runtime::create(std::move(config));
  CO_REQUIRE_OK(created);
  auto& runtime = *created.value();

  const std::string directory = runtime.config().state_directory;
  const EvidenceSubject link = cotest::link_subject();
  std::atomic<int> errors{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> finite_threads;
  std::vector<std::thread> looping_threads;

  // Writers: three independent sources, each with its own sequence space.
  for (int w = 0; w < 3; ++w) {
    finite_threads.emplace_back([&, w]() {
      cotest::Source source =
          cotest::make_source("writer-" + std::to_string(w), topology.generation());
      for (int i = 0; i < 60; ++i) {
        if (!runtime.ingest(source.ratio(EvidenceKind::kDropCount, link, 0.01, cotest::at(0)),
                            cotest::at(0))
                 .ok()) {
          errors.fetch_add(1);
        }
      }
    });
  }
  // persistence: save and load repeatedly.
  for (int p = 0; p < 2; ++p) {
    finite_threads.emplace_back([&]() {
      for (int i = 0; i < 4; ++i) {
        if (!runtime.save().ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.load().ok()) {
          errors.fetch_add(1);
        }
      }
    });
  }
  // analysts: the whole read surface, until the writers are done.
  for (int a = 0; a < 2; ++a) {
    looping_threads.emplace_back([&]() {
      while (!stop.load()) {
        if (!runtime.classify(link, Duration::from_seconds(60), cotest::at(1)).ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.localize(link, Duration::from_seconds(60), cotest::at(1)).ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.evaluate(link, Duration::from_seconds(60), cotest::at(1), nullptr).ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.correlate(cotest::at(1)).ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.explain(link, Duration::from_seconds(60), cotest::at(1)).ok()) {
          errors.fetch_add(1);
        }
        if (!runtime.export_json(ExportRequest{}, cotest::at(1)).ok()) {
          errors.fetch_add(1);
        }
        (void)runtime.metrics();
        (void)runtime.episodes(EpisodeFilter{});
      }
    });
  }

  // Finite work is joined first; only then are the looping analysts told to stop.
  for (std::thread& thread : finite_threads) {
    thread.join();
  }
  stop.store(true);
  for (std::thread& thread : looping_threads) {
    thread.join();
  }

  CO_EXPECT_EQ(errors.load(), 0);
  CO_EXPECT(runtime.metrics().evidence_retained >= 180);
  cotest::remove_tree(directory);
}

CO_TEST(hardening, snapshots_with_no_sections_are_refused) {
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  SnapshotContent content;
  content.metadata.policy_version = ClassificationPolicy{}.version;
  content.metadata.policy_digest = ClassificationPolicy{}.digest();
  content.topology = topology;
  auto encoded = encode_snapshot(content, limits);
  CO_REQUIRE_OK(encoded);
  std::vector<std::uint8_t> bytes = encoded.value();
  // Section count lives after magic(8) + major(4) + minor(4) + three digests(24) + created(8).
  const std::size_t section_count_offset = 8 + 4 + 4 + 8 + 8 + 8 + 8;
  for (int i = 0; i < 4; ++i) {
    bytes[section_count_offset + static_cast<std::size_t>(i)] = 0;
  }
  // A zero section count no longer matches the declared payload, so it must be refused.
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);
}

CO_TEST(hardening, repeated_restart_cycles_converge) {
  const std::string directory = cotest::unique_temp_dir("hardening-restart");
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  const EvidenceSubject link = cotest::link_subject();
  EpisodeId last_id;
  std::size_t last_episodes = 0;

  for (int cycle = 0; cycle < 4; ++cycle) {
    RuntimeConfig config = cotest::make_runtime_config(topology, cotest::test_limits());
    config.state_directory = directory;
    config.state_basename = "state";
    auto created = Runtime::create(std::move(config));
    CO_REQUIRE_OK(created);
    auto& runtime = *created.value();
    CO_REQUIRE_OK(runtime.load());

    cotest::Source source =
        cotest::make_source("collector-" + std::to_string(cycle), topology.generation());
    EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, link, 0.01, cotest::at(0));
    CO_REQUIRE_OK(runtime.ingest(record, cotest::at(0)));
    EpisodeUpdate update;
    CO_REQUIRE_OK(runtime.evaluate(link, Duration::from_seconds(10), cotest::at(1), &update));
    CO_REQUIRE_OK(runtime.save());

    const auto episodes = runtime.episodes(EpisodeFilter{});
    CO_EXPECT(episodes.size() >= last_episodes);
    last_episodes = episodes.size();
    if (!episodes.empty()) {
      last_id = episodes.front().id;
    }
    // Recovered history never makes a source live again.
    CO_EXPECT_EQ(runtime.metrics().live_sources, static_cast<std::size_t>(1));
  }
  CO_EXPECT(last_episodes >= 1);
  CO_EXPECT(last_id.valid());
  cotest::remove_tree(directory);
}