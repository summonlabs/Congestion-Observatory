// Congestion Observatory - explicit, checkable resource bounds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_LIMITS_HPP
#define CONGESTION_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "congestion/core/hash.hpp"
#include "congestion/core/result.hpp"

namespace congestion {

// Limits bound every growth vector of the runtime: ingested evidence, topology size, analysis
// fan-out, episode history, persistence size and worker/queue depth. A Limits value is part of
// the persisted snapshot identity: a snapshot written under different limits is reported as a
// version mismatch rather than silently truncated on load.
struct Limits {
  // ---- ingestion ------------------------------------------------------------
  std::size_t max_sources = 512;
  std::size_t max_retained_evidence = 200000;
  std::size_t max_evidence_per_subject = 8192;
  std::size_t max_metadata_entries = 16;
  std::size_t max_metadata_key_bytes = 48;
  std::size_t max_metadata_value_bytes = 128;
  std::size_t max_note_bytes = 256;
  std::size_t max_labels = 16;

  // ---- wire / documents -----------------------------------------------------
  std::size_t max_frame_bytes = 1u << 20;  // 1 MiB
  std::size_t max_json_depth = 32;
  std::size_t max_json_nodes = 100000;
  std::size_t max_document_bytes = 8u << 20;  // 8 MiB

  // ---- topology -------------------------------------------------------------
  std::size_t max_nodes = 4096;
  std::size_t max_ports = 16384;
  std::size_t max_links = 16384;
  std::size_t max_queues = 65536;
  std::size_t max_buffers = 16384;
  std::size_t max_paths = 16384;
  std::size_t max_flows = 65536;
  std::size_t max_path_hops = 64;
  std::size_t max_ports_per_node = 512;

  // ---- analysis -------------------------------------------------------------
  std::size_t max_window_records = 8192;
  std::size_t max_citations = 64;
  std::size_t max_explanation_steps = 128;
  std::size_t max_result_candidates = 16;
  std::size_t max_localization_depth = 8;
  std::size_t max_graph_nodes = 8192;
  std::size_t max_graph_edges = 32768;
  std::size_t max_blockers = 32;

  // ---- episodes and correlation --------------------------------------------
  std::size_t max_episodes = 4096;
  std::size_t max_episode_assessments = 64;
  std::size_t max_episode_transitions = 512;
  std::size_t max_groups = 1024;
  std::size_t max_group_members = 64;
  std::size_t max_episode_filter_results = 1024;

  // ---- runtime --------------------------------------------------------------
  std::size_t worker_threads = 2;  // 0 == synchronous, no background threads
  std::size_t max_pending_tasks = 1024;
  std::size_t max_ingest_queue_depth = 4096;

  // ---- persistence ----------------------------------------------------------
  std::size_t max_snapshot_bytes = 64u << 20;  // 64 MiB
  std::size_t max_snapshot_sections = 32;
  std::size_t max_snapshot_evidence = 100000;
  std::size_t max_snapshot_episodes = 4096;

  // Validates internal consistency (e.g. per-subject cap cannot exceed global cap).
  [[nodiscard]] Status validate() const;

  // Deterministic digest of every bound. Used as part of the snapshot compatibility check.
  [[nodiscard]] std::uint64_t digest() const noexcept;

  // Stable textual rendering, one "name=value" per line, sorted by name.
  [[nodiscard]] std::string describe() const;
};

// A Limits value with pathological bounds, used by adversarial tests to prove that limits are
// enforced rather than assumed. Not intended for production use.
[[nodiscard]] Limits minimal_limits();

}  // namespace congestion

#endif  // CONGESTION_CORE_LIMITS_HPP
