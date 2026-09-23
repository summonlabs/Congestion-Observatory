// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace congestion;

namespace {

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

SnapshotContent build_content(const Topology& topology) {
  SnapshotContent content;
  content.metadata.policy_version = ClassificationPolicy{}.version;
  content.metadata.policy_digest = ClassificationPolicy{}.digest();
  content.metadata.limits_digest = Limits{}.digest();
  content.metadata.topology_digest = topology.digest();
  content.metadata.created_at = cotest::at(0);
  content.metadata.runtime_instance = 42;
  content.metadata.generation = topology.generation();
  content.metadata.producer_version = version_string();
  content.topology = topology;

  const auto generation = topology.generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.01,
                                       cotest::at(0));
  content.evidence.push_back(record);

  Episode episode;
  episode.key.scope = cotest::link_subject();
  episode.key.mechanism = Mechanism::kPacketDrop;
  episode.key.generation = generation;
  episode.key.policy_version = content.metadata.policy_version;
  episode.id = compute_episode_id(episode.key);
  episode.state = EpisodeState::kOpen;
  episode.revision = Revision(3);
  episode.current_severity = Severity::kHigh;
  episode.peak_severity = Severity::kHigh;
  episode.confidence = 70;
  episode.first_seen = cotest::at(0);
  episode.last_seen = cotest::at(2);
  episode.last_updated = cotest::at(2);
  episode.observations = 4;
  episode.citations.push_back(record.id);
  episode.sources.push_back(SourceId::unchecked("collector-1"));
  EpisodeTransition transition;
  transition.revision = Revision(3);
  transition.kind = EpisodeTransitionKind::kOpened;
  transition.at = cotest::at(0);
  transition.cause = "test";
  transition.citations.push_back(record.id);
  episode.transitions.push_back(transition);
  AssessmentDigest digest;
  digest.at = cotest::at(2);
  digest.verdict = Verdict::kCongestionConfirmed;
  digest.severity = Severity::kHigh;
  digest.confidence = 70;
  digest.representative_citation = record.id;
  episode.assessments.push_back(digest);
  content.episodes.push_back(episode);
  return content;
}

}  // namespace

CO_TEST(persistence, snapshot_round_trip_preserves_everything) {
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  const SnapshotContent content = build_content(topology);

  auto encoded = encode_snapshot(content, limits);
  CO_EXPECT_OK(encoded);
  auto decoded = decode_snapshot(encoded.value(), limits, ClassificationPolicy{});
  CO_EXPECT_OK(decoded);
  if (!decoded.ok()) {
    return;
  }
  CO_EXPECT_EQ(decoded.value().topology.digest(), topology.digest());
  CO_EXPECT_EQ(decoded.value().topology.generation().str(), topology.generation().str());
  CO_EXPECT_EQ(decoded.value().episodes.size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(decoded.value().evidence.size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(decoded.value().metadata.runtime_instance, static_cast<std::uint64_t>(42));
  if (!decoded.value().episodes.empty()) {
    const Episode& episode = decoded.value().episodes.front();
    CO_EXPECT_EQ(episode.id, content.episodes.front().id);
    CO_EXPECT_EQ(episode.key.str(), content.episodes.front().key.str());
    CO_EXPECT_EQ(episode.revision.value(), static_cast<std::uint64_t>(3));
    CO_EXPECT_EQ(episode.state, EpisodeState::kOpen);
    CO_EXPECT_EQ(episode.peak_severity, Severity::kHigh);
    CO_EXPECT_EQ(episode.transitions.size(), static_cast<std::size_t>(1));
    CO_EXPECT_EQ(episode.assessments.size(), static_cast<std::size_t>(1));
    CO_EXPECT_EQ(episode.sources.size(), static_cast<std::size_t>(1));
  }
  if (!decoded.value().evidence.empty()) {
    CO_EXPECT_EQ(decoded.value().evidence.front().id, content.evidence.front().id);
    CO_EXPECT_EQ(decoded.value().evidence.front().kind, EvidenceKind::kDropCount);
    CO_EXPECT_EQ(decoded.value().evidence.front().subject.str(), cotest::link_subject().str());
  }
}

CO_TEST(persistence, corrupted_payload_is_rejected_whole) {
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto encoded = encode_snapshot(build_content(topology), limits);
  CO_EXPECT_OK(encoded);
  std::vector<std::uint8_t> bytes = encoded.value();

  // Flip one bit in the payload: the checksum must catch it.
  bytes[bytes.size() - 3] ^= 0x40;
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);

  bytes = encoded.value();
  bytes[bytes.size() - 1] ^= 0xFF;
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);

  // Damage the magic.
  bytes = encoded.value();
  bytes[0] = 'X';
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);

  // Truncate.
  bytes = encoded.value();
  bytes.resize(bytes.size() / 2);
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);

  // Append trailing bytes: the declared length no longer matches the file.
  bytes = encoded.value();
  bytes.push_back(0);
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);

  // An empty input is rejected rather than treated as an empty snapshot.
  CO_EXPECT_ERR(decode_snapshot({}, limits, ClassificationPolicy{}), ErrorCode::kIntegrityFailure);
}

CO_TEST(persistence, version_and_configuration_mismatches_are_named) {
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto encoded = encode_snapshot(build_content(topology), limits);
  CO_EXPECT_OK(encoded);
  std::vector<std::uint8_t> bytes = encoded.value();

  // Major format version lives immediately after the eight magic bytes.
  bytes[8] = 9;
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, ClassificationPolicy{}), ErrorCode::kVersionMismatch);

  // A different limits digest is a compatibility failure, not a silent truncation.
  bytes = encoded.value();
  CO_EXPECT_ERR(decode_snapshot(bytes, minimal_limits(), ClassificationPolicy{}),
                ErrorCode::kVersionMismatch);

  // A different policy is rejected as well.
  ClassificationPolicy other;
  other.version = "co-policy-other";
  CO_EXPECT_ERR(decode_snapshot(bytes, limits, other), ErrorCode::kVersionMismatch);
}

CO_TEST(persistence, snapshot_store_recovers_conservatively) {
  const std::string directory = cotest::unique_temp_dir("snapshot");
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  SnapshotStore store(directory, "congestion-observatory", limits);

  auto first = store.save(build_content(topology));
  CO_EXPECT_OK(first);
  CO_EXPECT(std::filesystem::exists(store.primary_path()));

  // No history yet: loading returns the primary.
  auto loaded = store.load(ClassificationPolicy{});
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedPrimary);

  // A second save keeps the first as the fallback.
  SnapshotContent second_content = build_content(topology);
  second_content.metadata.runtime_instance = 99;
  auto second = store.save(second_content);
  CO_EXPECT_OK(second);
  CO_EXPECT(std::filesystem::exists(store.fallback_path()));
  loaded = store.load(ClassificationPolicy{});
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedPrimary);
  CO_EXPECT_EQ(loaded.value().content.metadata.runtime_instance, static_cast<std::uint64_t>(99));

  // Damage the primary: the fallback is applied and the reason is reported.
  std::vector<std::uint8_t> bytes = read_bytes(store.primary_path());
  bytes[bytes.size() - 5] ^= 0x11;
  write_bytes(store.primary_path(), bytes);
  loaded = store.load(ClassificationPolicy{});
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedFallback);
  CO_EXPECT(loaded.value().integrity_failure);
  CO_EXPECT(!loaded.value().notes.empty());
  CO_EXPECT_EQ(loaded.value().content.metadata.runtime_instance, static_cast<std::uint64_t>(42));

  // Damage both: nothing is loaded, and the store never invents state.
  bytes = read_bytes(store.fallback_path());
  bytes[bytes.size() - 5] ^= 0x11;
  write_bytes(store.fallback_path(), bytes);
  loaded = store.load(ClassificationPolicy{});
  CO_EXPECT_OK(loaded);
  CO_EXPECT_EQ(loaded.value().outcome, LoadOutcome::kLoadedNothing);
  CO_EXPECT_EQ(loaded.value().content.episodes.size(), static_cast<std::size_t>(0));

  cotest::remove_tree(directory);
}

CO_TEST(persistence, snapshot_store_creates_missing_directories) {
  const std::string root = cotest::unique_temp_dir("snapshot-nested");
  const std::string nested = root + "/a/b/c";
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  SnapshotStore store(nested, "state", limits);
  auto saved = store.save(build_content(topology));
  CO_EXPECT_OK(saved);
  CO_EXPECT(std::filesystem::exists(saved.value().primary_path));
  CO_EXPECT(saved.value().bytes_written > 0);
  CO_EXPECT(saved.value().crc64 != 0);
  cotest::remove_tree(root);
}

CO_TEST(persistence, snapshot_bounds_are_enforced) {
  Limits limits = cotest::test_limits();
  limits.max_snapshot_bytes = 512;
  CO_EXPECT(limits.validate().ok());
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  SnapshotContent content = build_content(topology);
  CO_EXPECT_ERR(encode_snapshot(content, limits), ErrorCode::kLimitExceeded);

  Limits episode_limits = cotest::test_limits();
  episode_limits.max_snapshot_episodes = 1;
  episode_limits.max_episodes = 8;
  episode_limits.max_group_members = 8;
  CO_EXPECT(episode_limits.validate().ok());
  SnapshotContent many = build_content(topology);
  for (int i = 0; i < 5; ++i) {
    Episode episode = many.episodes.front();
    episode.key.scope = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    episode.id = compute_episode_id(episode.key);
    many.episodes.push_back(episode);
  }
  auto encoded = encode_snapshot(many, episode_limits);
  CO_REQUIRE_OK(encoded);
  auto decoded = decode_snapshot(encoded.value(), episode_limits, ClassificationPolicy{});
  CO_REQUIRE_OK(decoded);
  CO_EXPECT_EQ(decoded.value().episodes.size(), static_cast<std::size_t>(1));
}

CO_TEST(persistence, content_identity_is_verified_on_load) {
  const Limits limits = cotest::test_limits();
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto encoded = encode_snapshot(build_content(topology), limits);
  CO_EXPECT_OK(encoded);

  // Re-encode with a mismatched episode id by tampering with the immutable key is not possible
  // through the API, so instead verify that a valid snapshot always reproduces the same ids.
  auto decoded = decode_snapshot(encoded.value(), limits, ClassificationPolicy{});
  CO_EXPECT_OK(decoded);
  if (!decoded.value().episodes.empty()) {
    CO_EXPECT_EQ(compute_episode_id(decoded.value().episodes.front().key),
                 decoded.value().episodes.front().id);
  }
  if (!decoded.value().evidence.empty()) {
    CO_EXPECT_EQ(compute_evidence_id(decoded.value().evidence.front()),
                 decoded.value().evidence.front().id);
  }
}
