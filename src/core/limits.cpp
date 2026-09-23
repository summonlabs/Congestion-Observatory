// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/limits.hpp"

#include <string>

namespace congestion {
namespace {

Status require_positive(std::size_t value, const char* name) {
  if (value == 0) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             std::string("limit must be positive: ") + name));
  }
  return Status{};
}

}  // namespace

Status Limits::validate() const {
  if (worker_threads > 64) {
    return Status(make_error(ErrorCode::kOutOfRange, "worker_threads exceeds the supported maximum",
                             "worker_threads=" + std::to_string(worker_threads)));
  }
  const std::size_t positives[] = {
      max_sources,        max_retained_evidence, max_evidence_per_subject, max_metadata_key_bytes,
      max_metadata_value_bytes, max_note_bytes, max_frame_bytes, max_json_depth, max_json_nodes,
      max_document_bytes, max_nodes, max_ports, max_links, max_queues, max_buffers, max_paths,
      max_flows, max_path_hops, max_ports_per_node, max_window_records, max_citations,
      max_explanation_steps, max_result_candidates, max_localization_depth, max_graph_nodes,
      max_graph_edges, max_blockers, max_episodes, max_episode_assessments,
      max_episode_transitions, max_groups, max_group_members, max_episode_filter_results,
      max_pending_tasks, max_ingest_queue_depth, max_snapshot_bytes, max_snapshot_sections,
      max_snapshot_evidence, max_snapshot_episodes};
  const char* names[] = {
      "max_sources",        "max_retained_evidence", "max_evidence_per_subject",
      "max_metadata_key_bytes", "max_metadata_value_bytes", "max_note_bytes",
      "max_frame_bytes",    "max_json_depth",       "max_json_nodes",
      "max_document_bytes", "max_nodes",            "max_ports",
      "max_links",          "max_queues",           "max_buffers",
      "max_paths",          "max_flows",            "max_path_hops",
      "max_ports_per_node", "max_window_records",   "max_citations",
      "max_explanation_steps", "max_result_candidates", "max_localization_depth",
      "max_graph_nodes",    "max_graph_edges",      "max_blockers",
      "max_episodes",       "max_episode_assessments", "max_episode_transitions",
      "max_groups",         "max_group_members",    "max_episode_filter_results",
      "max_pending_tasks",  "max_ingest_queue_depth", "max_snapshot_bytes",
      "max_snapshot_sections", "max_snapshot_evidence", "max_snapshot_episodes"};
  static_assert(sizeof(positives) / sizeof(positives[0]) == sizeof(names) / sizeof(names[0]),
                "limit name table must match the value table");
  for (std::size_t i = 0; i < sizeof(positives) / sizeof(positives[0]); ++i) {
    const Status status = require_positive(positives[i], names[i]);
    if (!status.ok()) {
      return status;
    }
  }
  if (max_evidence_per_subject > max_retained_evidence) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "max_evidence_per_subject exceeds max_retained_evidence"));
  }
  if (max_window_records > max_retained_evidence) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "max_window_records exceeds max_retained_evidence"));
  }
  if (max_group_members > max_episodes) {
    return Status(make_error(ErrorCode::kInvalidArgument, "max_group_members exceeds max_episodes"));
  }
  if (max_snapshot_episodes > max_episodes) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "max_snapshot_episodes exceeds max_episodes"));
  }
  if (max_snapshot_evidence > max_retained_evidence) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "max_snapshot_evidence exceeds max_retained_evidence"));
  }
  if (max_localization_depth > 32) {
    return Status(make_error(ErrorCode::kOutOfRange, "max_localization_depth exceeds 32"));
  }
  return Status{};
}

std::uint64_t Limits::digest() const noexcept {
  StableHasher hasher;
  const std::size_t values[] = {
      max_sources,        max_retained_evidence, max_evidence_per_subject, max_metadata_entries,
      max_metadata_key_bytes, max_metadata_value_bytes, max_note_bytes, max_labels,
      max_frame_bytes,    max_json_depth,        max_json_nodes,           max_document_bytes,
      max_nodes,          max_ports,             max_links,                max_queues,
      max_buffers,        max_paths,             max_flows,                max_path_hops,
      max_ports_per_node, max_window_records,    max_citations,            max_explanation_steps,
      max_result_candidates, max_localization_depth, max_graph_nodes,      max_graph_edges,
      max_blockers,       max_episodes,          max_episode_assessments,  max_episode_transitions,
      max_groups,         max_group_members,     max_episode_filter_results,
      worker_threads,     max_pending_tasks,     max_ingest_queue_depth,   max_snapshot_bytes,
      max_snapshot_sections, max_snapshot_evidence, max_snapshot_episodes};
  for (const std::size_t value : values) {
    hasher.update_u64(static_cast<std::uint64_t>(value));
  }
  return hasher.digest64();
}

std::string Limits::describe() const {
  std::string out;
  const auto add = [&out](const char* name, std::size_t value) {
    out += name;
    out += "=";
    out += std::to_string(value);
    out += "\n";
  };
  add("max_sources", max_sources);
  add("max_retained_evidence", max_retained_evidence);
  add("max_evidence_per_subject", max_evidence_per_subject);
  add("max_metadata_entries", max_metadata_entries);
  add("max_frame_bytes", max_frame_bytes);
  add("max_document_bytes", max_document_bytes);
  add("max_json_depth", max_json_depth);
  add("max_json_nodes", max_json_nodes);
  add("max_nodes", max_nodes);
  add("max_links", max_links);
  add("max_queues", max_queues);
  add("max_paths", max_paths);
  add("max_flows", max_flows);
  add("max_window_records", max_window_records);
  add("max_citations", max_citations);
  add("max_result_candidates", max_result_candidates);
  add("max_localization_depth", max_localization_depth);
  add("max_graph_nodes", max_graph_nodes);
  add("max_graph_edges", max_graph_edges);
  add("max_episodes", max_episodes);
  add("max_episode_transitions", max_episode_transitions);
  add("max_groups", max_groups);
  add("max_group_members", max_group_members);
  add("worker_threads", worker_threads);
  add("max_pending_tasks", max_pending_tasks);
  add("max_ingest_queue_depth", max_ingest_queue_depth);
  add("max_snapshot_bytes", max_snapshot_bytes);
  add("max_snapshot_evidence", max_snapshot_evidence);
  add("max_snapshot_episodes", max_snapshot_episodes);
  if (!out.empty()) {
    out.pop_back();
  }
  return out;
}

Limits minimal_limits() {
  Limits limits;
  limits.max_sources = 4;
  limits.max_retained_evidence = 32;
  limits.max_evidence_per_subject = 16;
  limits.max_metadata_entries = 2;
  limits.max_labels = 2;
  limits.max_frame_bytes = 4096;
  limits.max_document_bytes = 4096;
  limits.max_json_depth = 8;
  limits.max_json_nodes = 512;
  limits.max_nodes = 8;
  limits.max_ports = 16;
  limits.max_links = 8;
  limits.max_queues = 8;
  limits.max_buffers = 8;
  limits.max_paths = 8;
  limits.max_flows = 8;
  limits.max_ports_per_node = 4;
  limits.max_window_records = 32;
  limits.max_citations = 8;
  limits.max_explanation_steps = 16;
  limits.max_result_candidates = 4;
  limits.max_localization_depth = 2;
  limits.max_graph_nodes = 16;
  limits.max_graph_edges = 32;
  limits.max_blockers = 8;
  limits.max_episodes = 8;
  limits.max_episode_assessments = 4;
  limits.max_episode_transitions = 8;
  limits.max_groups = 4;
  limits.max_group_members = 4;
  limits.max_episode_filter_results = 8;
  limits.worker_threads = 0;
  limits.max_pending_tasks = 16;
  limits.max_ingest_queue_depth = 16;
  limits.max_snapshot_bytes = 65536;
  limits.max_snapshot_sections = 8;
  limits.max_snapshot_evidence = 32;
  limits.max_snapshot_episodes = 8;
  return limits;
}

}  // namespace congestion
