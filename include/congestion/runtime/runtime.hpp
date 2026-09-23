// Congestion Observatory - the runtime facade.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_RUNTIME_RUNTIME_HPP
#define CONGESTION_RUNTIME_RUNTIME_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "congestion/assessment/classify.hpp"
#include "congestion/correlation/group.hpp"
#include "congestion/core/cancel.hpp"
#include "congestion/episode/episode.hpp"
#include "congestion/evidence/store.hpp"
#include "congestion/localization/localize.hpp"
#include "congestion/model/topology.hpp"
#include "congestion/persistence/store.hpp"

namespace congestion {

struct RuntimeConfig {
  Limits limits{};
  ClassificationPolicy classification{};
  CorrelationPolicy correlation{};
  LocalizationPolicy localization{};
  EpisodePolicy episode{};
  IngestPolicy ingest{};
  Topology topology{};
  std::string state_directory{};   // empty: persistence disabled
  std::string state_basename{"congestion-observatory"};
  bool start_workers{false};
};

struct ExportRequest {
  bool include_topology{true};
  bool include_episodes{true};
  bool include_evidence{false};
  bool include_metrics{true};
  std::size_t max_episodes{256};
  std::size_t max_evidence{1024};
};

struct RuntimeMetrics {
  std::uint64_t ingested{0};
  std::uint64_t ingest_rejected_fence{0};
  std::uint64_t ingest_rejected_stale{0};
  std::uint64_t ingest_rejected_replay{0};
  std::uint64_t ingest_rejected_authority{0};
  std::uint64_t ingest_rejected_invalid{0};
  std::uint64_t ingest_rejected_unknown_subject{0};
  std::uint64_t evidence_evicted{0};
  std::uint64_t classifications{0};
  std::uint64_t classifiable_unknown_subject{0};
  std::uint64_t localizations{0};
  std::uint64_t localizations_ambiguous{0};
  std::uint64_t episodes_opened{0};
  std::uint64_t episodes_escalated{0};
  std::uint64_t episodes_resolved{0};
  std::uint64_t episodes_expired{0};
  std::uint64_t correlations_run{0};
  std::uint64_t tasks_submitted{0};
  std::uint64_t tasks_completed{0};
  std::uint64_t tasks_rejected{0};
  std::uint64_t maintenance_runs{0};
  std::uint64_t snapshots_saved{0};
  std::uint64_t snapshots_loaded{0};
  std::uint64_t snapshot_failures{0};
  std::uint64_t liveness_resets{0};
  std::size_t evidence_retained{0};
  std::size_t episodes_tracked{0};
  std::size_t live_sources{0};
  std::size_t known_sources{0};
  bool workers_running{false};
  bool shutting_down{false};
};

// The runtime owns every stateful component. Lock discipline: the runtime never holds two
// component locks at once. It snapshots under one lock, releases it, then works on the copy.
class Runtime {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Runtime>> create(RuntimeConfig config);
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // ---- ingestion ------------------------------------------------------------
  [[nodiscard]] Result<IngestOutcome> ingest(const EvidenceRecord& record, Timestamp now);
  [[nodiscard]] Result<IngestOutcome> ingest_recovered(const EvidenceRecord& record);

  // ---- analysis -------------------------------------------------------------
  [[nodiscard]] Result<CongestionAssessment> classify(const EvidenceSubject& subject,
                                                      Duration window, Timestamp now) const;
  [[nodiscard]] Result<CongestionAssessment> classify_with(const EvidenceSubject& subject,
                                                           Timestamp window_start,
                                                           Timestamp window_end,
                                                           Timestamp now) const;
  [[nodiscard]] Result<LocalizationResult> localize(const EvidenceSubject& subject, Duration window,
                                                    Timestamp now) const;
  [[nodiscard]] Result<std::vector<CorrelationGroup>> correlate(Timestamp now) const;

  // Classifies, updates episodes, then localizes if congestion is asserted.
  [[nodiscard]] Result<CongestionAssessment> evaluate(const EvidenceSubject& subject,
                                                      Duration window, Timestamp now,
                                                      EpisodeUpdate* update_out);

  // ---- episodes -------------------------------------------------------------
  [[nodiscard]] std::vector<Episode> episodes(const EpisodeFilter& filter) const;
  [[nodiscard]] Result<Episode> episode(const EpisodeId& id) const;
  [[nodiscard]] Result<std::vector<EpisodeUpdate>> advance_episodes(Timestamp now);

  // ---- explanation ----------------------------------------------------------
  [[nodiscard]] Result<Explanation> explain(const EvidenceSubject& subject, Duration window,
                                            Timestamp now) const;
  [[nodiscard]] Result<Explanation> explain_episode(const EpisodeId& id) const;

  // ---- export ---------------------------------------------------------------
  [[nodiscard]] Result<std::string> export_json(const ExportRequest& request, Timestamp now) const;

  // ---- topology -------------------------------------------------------------
  // The topology is held behind a shared pointer so that analysis never observes a partially
  // replaced topology. The reference returned here stays valid until the next admit_topology().
  [[nodiscard]] const Topology& topology() const noexcept { return *topology_; }
  [[nodiscard]] Status admit_topology(Topology topology);
  [[nodiscard]] const CausalGraph& causal_graph() const noexcept { return graph_; }
  [[nodiscard]] Status rebuild_causal_graph();
  // Registers an observational causal edge. Rejected without citations.
  [[nodiscard]] Status add_causal_edge(const CausalEdge& edge);

  // ---- persistence ----------------------------------------------------------
  [[nodiscard]] Result<SaveReport> save();
  [[nodiscard]] Result<LoadReport> load();
  [[nodiscard]] bool persistence_enabled() const noexcept { return !config_.state_directory.empty(); }

  // ---- lifecycle ------------------------------------------------------------
  [[nodiscard]] Status start_workers();
  [[nodiscard]] Status stop_workers();
  void request_shutdown();
  [[nodiscard]] bool shutting_down() const noexcept;
  [[nodiscard]] CancellationToken cancellation_token() const noexcept { return cancel_.token(); }

  // Runs one maintenance pass synchronously: expires episodes, sweeps evidence, refreshes
  // liveness bookkeeping. Returns the number of state changes.
  [[nodiscard]] Result<std::size_t> run_maintenance(Timestamp now);

  // Queues one maintenance pass on the worker pool. Returns kUnsupported when the pool is not
  // running; the synchronous run_maintenance() is always available.
  [[nodiscard]] Status request_maintenance();

  [[nodiscard]] RuntimeMetrics metrics() const;
  [[nodiscard]] const Limits& limits() const noexcept { return config_.limits; }
  [[nodiscard]] const ClassificationPolicy& policy() const noexcept { return config_.classification; }
  [[nodiscard]] const RuntimeConfig& config() const noexcept { return config_; }
  [[nodiscard]] const EvidenceStore& evidence() const noexcept { return *store_; }

 private:
  explicit Runtime(RuntimeConfig config);

  [[nodiscard]] std::vector<EvidenceRecord> window_records(const EvidenceSubject& subject,
                                                           Timestamp start, Timestamp end) const;
  // Returns the current topology snapshot. The shared pointer keeps it alive for the duration of
  // an analysis even if admit_topology() replaces it concurrently.
  [[nodiscard]] std::shared_ptr<const Topology> current_topology() const;

  RuntimeConfig config_{};
  mutable std::mutex config_mutex_;
  std::shared_ptr<const Topology> topology_{};
  CausalGraph graph_{};
  std::unique_ptr<EvidenceStore> store_{};
  mutable std::unique_ptr<EpisodeRegistry> registry_{};
  mutable std::unique_ptr<SnapshotStore> snapshot_store_{};
  CancellationSource cancel_{};
  struct WorkerPool;
  std::unique_ptr<WorkerPool> workers_{};
  mutable std::mutex metrics_mutex_;
  mutable RuntimeMetrics metrics_{};
  std::atomic<bool> shutting_down_{false};
};

}  // namespace congestion

#endif  // CONGESTION_RUNTIME_RUNTIME_HPP
