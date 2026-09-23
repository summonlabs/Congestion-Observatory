// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Restart and recovery: history survives, liveness does not.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace congestion;

namespace {

RuntimeConfig persistent_config(const Topology& topology, const std::string& directory) {
  RuntimeConfig config = cotest::make_runtime_config(topology, cotest::test_limits());
  config.state_directory = directory;
  config.state_basename = "state";
  return config;
}

void corrupt_byte(const std::string& path, std::size_t from_end) {
  std::ifstream input(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  input.close();
  if (bytes.size() <= from_end) {
    return;
  }
  bytes[bytes.size() - 1 - from_end] ^= 0x5A;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

CO_TEST(restart, history_survives_and_liveness_does_not) {
  const std::string directory = cotest::unique_temp_dir("restart");
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  const EvidenceSubject link = cotest::link_subject();

  EpisodeId episode_id;
  {
    auto created = Runtime::create(persistent_config(topology, directory));
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    auto& runtime = *created.value();
    cotest::Source source = cotest::make_source("collector-1", topology.generation());
    std::vector<EvidenceRecord> records;
    records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0)));
    records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
    for (const EvidenceRecord& record : records) {
      CO_EXPECT(runtime.ingest(record, cotest::at(0)).value().stored);
    }
    EpisodeUpdate update;
    CO_EXPECT(runtime.evaluate(link, Duration::from_seconds(10), cotest::at(1), &update).ok());
    CO_EXPECT_EQ(update.kind, EpisodeUpdateKind::kOpened);
    episode_id = update.id;
    CO_EXPECT_EQ(runtime.metrics().live_sources, static_cast<std::size_t>(1));
    CO_EXPECT(runtime.save().ok());
  }

  {
    auto created = Runtime::create(persistent_config(topology, directory));
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    auto& runtime = *created.value();
    CO_EXPECT_EQ(runtime.metrics().evidence_retained, static_cast<std::size_t>(0));
    auto loaded = runtime.load();
    CO_EXPECT_OK(loaded);
    for (const std::string& note : loaded.value().notes) {
      co_ctx.note(note);
    }
    CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedPrimary);
    CO_EXPECT_EQ(runtime.metrics().episodes_tracked, static_cast<std::size_t>(1));
    CO_EXPECT(runtime.metrics().evidence_retained >= 2);

    // Liveness is process local: after a restart no source is live.
    CO_EXPECT_EQ(runtime.metrics().live_sources, static_cast<std::size_t>(0));
    CO_EXPECT_EQ(runtime.metrics().known_sources, static_cast<std::size_t>(1));
    CO_EXPECT(!runtime.evidence().source_live(SourceId::unchecked("collector-1")));

    // The episode history is preserved with its identity and revision.
    auto episode = runtime.episode(episode_id);
    CO_EXPECT_OK(episode);
    if (episode.ok()) {
      CO_EXPECT_EQ(episode.value().id, episode_id);
      CO_EXPECT_EQ(episode.value().state, EpisodeState::kOpen);
      CO_EXPECT_EQ(episode.value().revision.value(), static_cast<std::uint64_t>(1));
      CO_EXPECT_EQ(episode.value().transitions.size(), static_cast<std::size_t>(1));
      CO_EXPECT_EQ(compute_episode_id(episode.value().key), episode_id);
    }

    // Recovered evidence must not silently become fresh: congestion cannot be re-asserted from
    // history alone.
    auto assessment = runtime.classify(link, Duration::from_seconds(3600), cotest::at(2));
    CO_EXPECT_OK(assessment);
    CO_EXPECT(!assessment.value().congestion_asserted);
    CO_EXPECT_EQ(assessment.value().verdict, Verdict::kIndeterminate);
    CO_EXPECT(assessment.value().has_blocker(blockers::kPersistedNotLive));
    CO_EXPECT_EQ(assessment.value().features.records_recovered, static_cast<std::size_t>(2));

    // A fresh observation from the same source is accepted, and only then is congestion asserted
    // again: the fence restarts from a first observation, it does not replay the old sequence.
    cotest::Source source = cotest::make_source("collector-1", topology.generation());
    EvidenceRecord fresh = source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(2));
    auto outcome = runtime.ingest(fresh, cotest::at(2));
    CO_EXPECT_OK(outcome);
    CO_EXPECT(outcome.value().stored);
    CO_EXPECT_EQ(outcome.value().fence, FenceDecision::kAcceptedFirstObservation);
    auto refreshed = runtime.classify(link, Duration::from_seconds(3600), cotest::at(3));
    CO_EXPECT_OK(refreshed);
    CO_EXPECT(refreshed.value().congestion_asserted);

    // The recovered evidence is still retained as history.
    CO_EXPECT(runtime.metrics().evidence_retained >= 3);
  }

  cotest::remove_tree(directory);
}

CO_TEST(restart, corrupt_primary_falls_back_to_the_previous_snapshot) {
  const std::string directory = cotest::unique_temp_dir("restart-fallback");
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  const EvidenceSubject link = cotest::link_subject();

  for (int generation = 0; generation < 2; ++generation) {
    auto created = Runtime::create(persistent_config(topology, directory));
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    auto& runtime = *created.value();
    cotest::Source source = cotest::make_source("collector-1", topology.generation());
    EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0));
    CO_EXPECT(runtime.ingest(record, cotest::at(0)).value().stored);
    CO_EXPECT(runtime.save().ok());
  }

  const std::string primary = directory + "/state.snapshot";
  CO_EXPECT(std::filesystem::exists(primary));
  corrupt_byte(primary, 4);

  auto created = Runtime::create(persistent_config(topology, directory));
  CO_EXPECT_OK(created);
  if (!created.ok()) {
    return;
  }
  auto loaded = created.value()->load();
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedFallback);
  CO_EXPECT(loaded.value().integrity_failure);
  CO_EXPECT(!loaded.value().notes.empty());
  CO_EXPECT_EQ(created.value()->metrics().live_sources, static_cast<std::size_t>(0));
  cotest::remove_tree(directory);
}

CO_TEST(restart, unusable_snapshots_leave_the_runtime_empty) {
  const std::string directory = cotest::unique_temp_dir("restart-broken");
  const Topology topology = cotest::make_test_topology(cotest::test_generation());

  auto created = Runtime::create(persistent_config(topology, directory));
  CO_EXPECT_OK(created);
  if (!created.ok()) {
    return;
  }
  auto& runtime = *created.value();
  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  CO_EXPECT(runtime
                .ingest(source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5,
                                     cotest::at(0)),
                        cotest::at(0))
                .value()
                .stored);
  CO_EXPECT(runtime.save().ok());
  CO_EXPECT(runtime.save().ok());

  corrupt_byte(directory + "/state.snapshot", 4);
  corrupt_byte(directory + "/state.snapshot.previous", 4);

  auto second = Runtime::create(persistent_config(topology, directory));
  CO_EXPECT_OK(second);
  if (!second.ok()) {
    return;
  }
  auto loaded = second.value()->load();
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedNothing);
  CO_EXPECT_EQ(second.value()->metrics().evidence_retained, static_cast<std::size_t>(0));
  CO_EXPECT_EQ(second.value()->metrics().episodes_tracked, static_cast<std::size_t>(0));
  cotest::remove_tree(directory);
}

CO_TEST(restart, policy_change_is_reported_rather_than_silently_applied) {
  const std::string directory = cotest::unique_temp_dir("restart-policy");
  const Topology topology = cotest::make_test_topology(cotest::test_generation());

  {
    auto created = Runtime::create(persistent_config(topology, directory));
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    CO_EXPECT(created.value()->save().ok());
  }
  {
    RuntimeConfig config = persistent_config(topology, directory);
    config.classification.version = "co-policy-changed";
    auto created = Runtime::create(config);
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    auto loaded = created.value()->load();
    CO_EXPECT_OK(loaded);
    CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedNothing);
    CO_EXPECT(loaded.value().version_mismatch);
  }
  {
    RuntimeConfig config = persistent_config(topology, directory);
    config.limits.max_episodes = cotest::test_limits().max_episodes - 1;
    config.limits.max_snapshot_episodes = config.limits.max_episodes;
    config.limits.max_group_members = config.limits.max_episodes;
    auto created = Runtime::create(config);
    CO_EXPECT_OK(created);
    if (!created.ok()) {
      return;
    }
    auto loaded = created.value()->load();
    CO_EXPECT_OK(loaded);
    CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedNothing);
    CO_EXPECT(loaded.value().version_mismatch);
  }
  cotest::remove_tree(directory);
}

CO_TEST(restart, without_persistence_saving_is_refused) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  CO_EXPECT(!runtime->persistence_enabled());
  CO_EXPECT_ERR(runtime->save(), ErrorCode::kUnsupported);
  CO_EXPECT_ERR(runtime->load(), ErrorCode::kUnsupported);
}
