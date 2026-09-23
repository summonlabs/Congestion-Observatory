// Congestion Observatory - versioned, integrity checked snapshot format.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_PERSISTENCE_SNAPSHOT_HPP
#define CONGESTION_PERSISTENCE_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "congestion/assessment/policy.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/episode/episode.hpp"
#include "congestion/evidence/evidence.hpp"
#include "congestion/model/topology.hpp"

namespace congestion {

// Snapshot layout (all integers little endian, all lengths checked before use):
//
//   magic[8] = "COSNAP01"
//   format_major u32, format_minor u32
//   limits_digest u64, policy_digest u64
//   topology_digest u64, created_at i64
//   section_count u32, payload_bytes u64, payload_crc64 u64
//   section table: section_id u32, offset u64, length u64, crc64 u64   (repeated)
//   payload bytes
//
// A snapshot is only applied when every structural check, the limits digest, the policy digest
// and every section CRC pass. Recovery is conservative: a damaged snapshot is rejected whole,
// never partially applied.
struct SnapshotSection {
  std::uint32_t id{0};
  std::uint64_t offset{0};
  std::uint64_t length{0};
  std::uint64_t crc64{0};
};

enum class SnapshotSectionId : std::uint32_t {
  kTopology = 1,
  kEpisodes = 2,
  kEvidence = 3,
  kMetadata = 4,
};

inline constexpr char kSnapshotMagic[8] = {'C', 'O', 'S', 'N', 'A', 'P', '0', '1'};
inline constexpr std::size_t kSnapshotHeaderBytes = 8 + 4 + 4 + 8 + 8 + 8 + 8 + 4 + 8 + 8;

struct SnapshotMetadata {
  std::string policy_version{};
  std::uint64_t policy_digest{0};
  std::uint64_t limits_digest{0};
  std::uint64_t topology_digest{0};
  Timestamp created_at{};
  std::uint64_t runtime_instance{0};
  std::uint64_t accepted_total{0};
  std::uint64_t rejected_total{0};
  GenerationVector generation{};
  std::string producer_version{};
};

struct SnapshotContent {
  SnapshotMetadata metadata{};
  Topology topology{};
  std::vector<Episode> episodes{};
  std::vector<EvidenceRecord> evidence{};
  bool evidence_truncated{false};
};

// Serialises a snapshot to bytes. Fails when the encoded size would exceed the configured bound.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_snapshot(const SnapshotContent& content,
                                                               const Limits& limits);

// Parses and fully validates a snapshot. Every section CRC is verified before the payload is
// decoded, so a corrupted section can never partially mutate caller state.
[[nodiscard]] Result<SnapshotContent> decode_snapshot(const std::vector<std::uint8_t>& bytes,
                                                       const Limits& limits,
                                                       const ClassificationPolicy& policy);

}  // namespace congestion

#endif  // CONGESTION_PERSISTENCE_SNAPSHOT_HPP
