// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/runtime/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "congestion/core/json.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

// Longest retention horizon applied by a maintenance sweep.
Duration retention_horizon(const ClassificationPolicy& policy) {
  return Duration(policy.freshness.stale_within.nanos() * 4);
}

}  // namespace

// Bounded worker pool. Tasks are executed to completion; shutdown drains the queue and joins every
// worker, so no task is ever abandoned. A task must never submit to this pool and then wait for
// its own submission to complete: that would be a self deadlock, and the pool is deliberately not
// reentrant. See docs/concurrency.md.
struct Runtime::WorkerPool {
  mutable std::mutex mutex{};
  std::condition_variable condition{};
  std::deque<std::function<void()>> tasks{};
  std::vector<std::thread> threads{};
  bool stopping{false};
  std::size_t max_pending{0};
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<std::uint64_t> submitted{0};

  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this]() { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          if (stopping) {
            return;
          }
          continue;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      // The lock is released before the task runs: the pool is never held while user code runs.
      task();
      completed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  Status start(std::size_t count, std::size_t pending_bound) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!threads.empty()) {
      return Status{};
    }
    stopping = false;
    max_pending = pending_bound;
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      threads.emplace_back([this]() { worker_loop(); });
    }
    return Status{};
  }

  Status submit(std::function<void()> task) {
    if (!task) {
      return Status(make_error(ErrorCode::kInvalidArgument, "empty task submitted"));
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (stopping) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return Status(make_error(ErrorCode::kShuttingDown, "worker pool is shutting down"));
      }
      if (threads.empty()) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return Status(make_error(ErrorCode::kUnsupported, "worker pool is not running"));
      }
      if (tasks.size() >= max_pending) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return Status(make_error(ErrorCode::kQueueFull, "worker queue is full",
                                 std::to_string(tasks.size())));
      }
      tasks.push_back(std::move(task));
      submitted.fetch_add(1, std::memory_order_relaxed);
    }
    condition.notify_one();
    return Status{};
  }

  void stop() {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    condition.notify_all();
    for (std::thread& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    threads.clear();
  }

  [[nodiscard]] bool running() const {
    const std::lock_guard<std::mutex> lock(mutex);
    return !threads.empty() && !stopping;
  }
};

Runtime::Runtime(RuntimeConfig config) : config_(std::move(config)) {}

Runtime::~Runtime() {
  request_shutdown();
  if (workers_ != nullptr) {
    workers_->stop();
  }
}

Result<std::unique_ptr<Runtime>> Runtime::create(RuntimeConfig config) {
  const Status limits_valid = config.limits.validate();
  if (!limits_valid.ok()) {
    return limits_valid.error();
  }
  const Status policy_valid = config.classification.validate();
  if (!policy_valid.ok()) {
    return policy_valid.error();
  }
  if (config.correlation.max_group_members == 0) {
    return make_error(ErrorCode::kInvalidArgument, "correlation group size must be positive");
  }
  if (config.localization.max_depth > config.limits.max_localization_depth) {
    return make_error(ErrorCode::kInvalidArgument,
                      "localization depth exceeds the configured limit");
  }
  if (config.limits.worker_threads > 0 && config.limits.max_pending_tasks == 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "worker threads require a positive pending task bound");
  }
  auto runtime = std::unique_ptr<Runtime>(new Runtime(std::move(config)));
  runtime->topology_ = std::make_shared<const Topology>(runtime->config_.topology);
  runtime->store_ = std::make_unique<EvidenceStore>(runtime->config_.limits);
  runtime->registry_ = std::make_unique<EpisodeRegistry>(runtime->config_.limits);
  if (runtime->persistence_enabled()) {
    runtime->snapshot_store_ = std::make_unique<SnapshotStore>(
        runtime->config_.state_directory, runtime->config_.state_basename, runtime->config_.limits);
  }
  const Status graph_status = runtime->rebuild_causal_graph();
  if (!graph_status.ok()) {
    return graph_status.error();
  }
  if (runtime->config_.start_workers) {
    const Status started = runtime->start_workers();
    if (!started.ok()) {
      return started.error();
    }
  }
  return runtime;
}

Result<IngestOutcome> Runtime::ingest(const EvidenceRecord& record, Timestamp now) {
  if (shutting_down_.load(std::memory_order_acquire)) {
    return make_error(ErrorCode::kShuttingDown, "runtime is shutting down");
  }
  const std::shared_ptr<const Topology> topology = current_topology();
  auto result = store_->ingest(record, now, config_.ingest, topology.get());
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  if (!result.ok()) {
    switch (result.error().code()) {
      case ErrorCode::kNotFound: ++metrics_.ingest_rejected_unknown_subject; break;
      default: ++metrics_.ingest_rejected_invalid; break;
    }
    return result.error();
  }
  const IngestOutcome& outcome = result.value();
  if (outcome.stored) {
    ++metrics_.ingested;
    metrics_.evidence_evicted += outcome.evicted;
    if (outcome.liveness_reset) {
      ++metrics_.liveness_resets;
    }
  } else {
    ++metrics_.ingest_rejected_fence;
    switch (outcome.fence) {
      case FenceDecision::kRejectedReplayedSequence:
        ++metrics_.ingest_rejected_replay;
        break;
      case FenceDecision::kRejectedStaleEpoch:
      case FenceDecision::kRejectedStaleGeneration:
      case FenceDecision::kRejectedStaleRevision:
      case FenceDecision::kRejectedStaleIncarnation:
      case FenceDecision::kRejectedIncomparableGeneration:
      case FenceDecision::kRejectedInconsistentBoot:
        ++metrics_.ingest_rejected_stale;
        break;
      case FenceDecision::kRejectedAuthorityTooLow:
        ++metrics_.ingest_rejected_authority;
        break;
      default:
        ++metrics_.ingest_rejected_invalid;
        break;
    }
  }
  return result;
}

Result<IngestOutcome> Runtime::ingest_recovered(const EvidenceRecord& record) {
  auto result = store_->ingest_recovered(record);
  if (!result.ok()) {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.ingest_rejected_invalid;
    return result.error();
  }
  return result;
}

std::vector<EvidenceRecord> Runtime::window_records(const EvidenceSubject& subject, Timestamp start,
                                                    Timestamp end) const {
  EvidenceQuery query;
  query.subject = &subject;
  query.window_start = start;
  query.window_end = end;
  query.max_records = config_.limits.max_window_records;
  query.include_recovered = true;
  query.include_non_live = true;
  return store_->query(query);
}

Result<CongestionAssessment> Runtime::classify_with(const EvidenceSubject& subject,
                                                    Timestamp window_start, Timestamp window_end,
                                                    Timestamp now) const {
  const std::shared_ptr<const Topology> topology = current_topology();
  if (!topology->contains(subject)) {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.classifiable_unknown_subject;
    return make_error(ErrorCode::kNotFound, "subject is not part of the admitted topology",
                      subject.str());
  }
  std::vector<EvidenceRecord> records = window_records(subject, window_start, window_end);

  TenantId tenant{};
  if (const FlowId* flow = subject.get_if<FlowId>()) {
    if (const Flow* model = topology->flow(*flow)) {
      tenant = model->tenant;
    }
  } else if (const TenantId* direct = subject.get_if<TenantId>()) {
    tenant = *direct;
  }

  ClassificationRequest request;
  request.subject = subject;
  request.window_start = window_start;
  request.window_end = window_end;
  request.evaluated_at = now;
  request.generation = topology->generation();
  request.tenant = tenant;
  request.max_citations = config_.limits.max_citations;

  std::vector<const EvidenceRecord*> pointers;
  pointers.reserve(records.size());
  for (const EvidenceRecord& record : records) {
    pointers.push_back(&record);
  }
  auto assessment = classify_subject(request, pointers, config_.classification);
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.classifications;
  return assessment;
}

Result<CongestionAssessment> Runtime::classify(const EvidenceSubject& subject, Duration window,
                                               Timestamp now) const {
  if (window <= Duration::zero()) {
    return make_error(ErrorCode::kInvalidArgument, "classification window must be positive");
  }
  return classify_with(subject, Timestamp(now.unix_nanos() - window.nanos()), now, now);
}

Result<LocalizationResult> Runtime::localize(const EvidenceSubject& subject, Duration window,
                                             Timestamp now) const {
  auto assessment = classify(subject, window, now);
  if (!assessment.ok()) {
    return assessment.error();
  }
  // Candidate scoring needs observations about the candidates, so the window is not restricted
  // to the symptom. The classifier ignores records for other subjects.
  EvidenceQuery query;
  query.max_records = config_.limits.max_window_records;
  query.window_start = Timestamp(now.unix_nanos() - window.nanos());
  query.window_end = now;
  std::vector<EvidenceRecord> records = store_->query(query);
  std::vector<const EvidenceRecord*> pointers;
  pointers.reserve(records.size());
  for (const EvidenceRecord& record : records) {
    pointers.push_back(&record);
  }
  LocalizationRequest request;
  request.symptom = subject;
  request.window_start = Timestamp(now.unix_nanos() - window.nanos());
  request.window_end = now;
  request.evaluated_at = now;
  request.generation = current_topology()->generation();
  request.assessment = &assessment.value();
  auto result = ::congestion::localize(request, graph_, pointers, config_.classification,
                                       config_.localization, config_.limits);
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.localizations;
  if (result.ok() && result.value().outcome == LocalizationOutcome::kAmbiguous) {
    ++metrics_.localizations_ambiguous;
  }
  return result;
}

Result<CongestionAssessment> Runtime::evaluate(const EvidenceSubject& subject, Duration window,
                                               Timestamp now, EpisodeUpdate* update_out) {
  auto assessment = classify(subject, window, now);
  if (!assessment.ok()) {
    return assessment.error();
  }
  auto update = registry_->observe(assessment.value(), config_.episode, now);
  if (!update.ok()) {
    return update.error();
  }
  if (update_out != nullptr) {
    *update_out = update.value();
  }
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  switch (update.value().kind) {
    case EpisodeUpdateKind::kOpened: ++metrics_.episodes_opened; break;
    case EpisodeUpdateKind::kEscalated: ++metrics_.episodes_escalated; break;
    case EpisodeUpdateKind::kResolved: ++metrics_.episodes_resolved; break;
    case EpisodeUpdateKind::kExpired: ++metrics_.episodes_expired; break;
    default: break;
  }
  return assessment;
}

std::vector<Episode> Runtime::episodes(const EpisodeFilter& filter) const {
  EpisodeFilter bounded = filter;
  if (bounded.max_results == 0 || bounded.max_results > config_.limits.max_episode_filter_results) {
    bounded.max_results = config_.limits.max_episode_filter_results;
  }
  return registry_->list(bounded);
}

Result<Episode> Runtime::episode(const EpisodeId& id) const { return registry_->get(id); }

Result<std::vector<EpisodeUpdate>> Runtime::advance_episodes(Timestamp now) {
  return registry_->advance(now, config_.episode, cancel_.token());
}

Result<std::vector<CorrelationGroup>> Runtime::correlate(Timestamp now) const {
  (void)now;
  std::vector<Episode> all = registry_->all();
  auto groups = ::congestion::correlate(all, config_.correlation, config_.limits, cancel_.token());
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.correlations_run;
  return groups;
}

Result<Explanation> Runtime::explain(const EvidenceSubject& subject, Duration window,
                                     Timestamp now) const {
  auto assessment = classify(subject, window, now);
  if (!assessment.ok()) {
    return assessment.error();
  }
  return assessment.value().explanation;
}

Result<Explanation> Runtime::explain_episode(const EpisodeId& id) const {
  auto episode = registry_->get(id);
  if (!episode.ok()) {
    return episode.error();
  }
  Explanation explanation;
  explanation.policy_version = config_.classification.version;
  explanation.policy_digest = config_.classification.digest();
  explanation.policy_digest_hex = to_hex(explanation.policy_digest);
  explanation.steps.push_back(ExplanationStep{
      "E00-identity", "applied", episode.value().key.str(), {}});
  for (const EpisodeTransition& transition : episode.value().transitions) {
    ExplanationStep step;
    step.rule_id = "E01-transition";
    step.decision = std::string(to_string(transition.kind));
    step.reason_code = transition.cause;
    step.citations = transition.citations;
    explanation.steps.push_back(std::move(step));
  }
  if (episode.value().history_truncated) {
    explanation.blockers.push_back(Blocker{
        "episode_history_truncated",
        std::to_string(episode.value().transitions_dropped) + " transition(s) and " +
            std::to_string(episode.value().assessments_dropped) +
            " assessment(s) were dropped at the front of the bounded history"});
  }
  return explanation;
}

Result<std::string> Runtime::export_json(const ExportRequest& request, Timestamp now) const {
  (void)now;
  JsonObject root;
  root.set("product", JsonValue(std::string(product_id())));
  root.set("version", JsonValue(version_string()));
  root.set("build", JsonValue(build_identity()));
  root.set("policy_version", JsonValue(config_.classification.version));
  root.set("policy_digest", JsonValue(to_hex(config_.classification.digest())));
  const std::shared_ptr<const Topology> topology = current_topology();
  root.set("topology_generation", JsonValue(topology->generation().str()));
  root.set("topology_digest", JsonValue(to_hex(topology->digest())));
  root.set("causal_graph_digest", JsonValue(to_hex(graph_.digest())));
  root.set("causal_graph_nodes", JsonValue(static_cast<std::int64_t>(graph_.node_count())));
  root.set("causal_graph_edges", JsonValue(static_cast<std::int64_t>(graph_.edge_count())));

  if (request.include_topology) {
    JsonObject topology_summary;
    topology_summary.set("generation", JsonValue(topology->generation().str()));
    topology_summary.set("nodes", JsonValue(static_cast<std::int64_t>(topology->nodes().size())));
    topology_summary.set("ports", JsonValue(static_cast<std::int64_t>(topology->ports().size())));
    topology_summary.set("links", JsonValue(static_cast<std::int64_t>(topology->links().size())));
    topology_summary.set("queues", JsonValue(static_cast<std::int64_t>(topology->queues().size())));
    topology_summary.set("buffers", JsonValue(static_cast<std::int64_t>(topology->buffers().size())));
    topology_summary.set("paths", JsonValue(static_cast<std::int64_t>(topology->paths().size())));
    topology_summary.set("flows", JsonValue(static_cast<std::int64_t>(topology->flows().size())));
    JsonValue::Array link_list;
    for (const auto& entry : topology->links()) {
      JsonObject link;
      link.set("id", JsonValue(entry.second.id.str()));
      link.set("endpoint_a", JsonValue(entry.second.endpoint_a.str()));
      link.set("endpoint_b", JsonValue(entry.second.endpoint_b.str()));
      link.set("capacity_bps", JsonValue(static_cast<std::int64_t>(entry.second.capacity_bps)));
      link_list.push_back(JsonValue(std::move(link)));
    }
    topology_summary.set("link_list", JsonValue(std::move(link_list)));
    root.set("topology", JsonValue(std::move(topology_summary)));
  }

  if (request.include_episodes) {
    JsonValue::Array episodes;
    const std::vector<Episode> all = registry_->all();
    const std::size_t bound = std::min(request.max_episodes, all.size());
    for (std::size_t i = 0; i < bound; ++i) {
      const Episode& episode = all[i];
      JsonObject item;
      item.set("id", JsonValue(episode.id.str()));
      item.set("scope", JsonValue(episode.key.scope.str()));
      item.set("tenant", JsonValue(episode.key.tenant.valid() ? episode.key.tenant.str()
                                                              : std::string("-")));
      item.set("mechanism", JsonValue(std::string(to_string(episode.key.mechanism))));
      item.set("generation", JsonValue(episode.key.generation.str()));
      item.set("state", JsonValue(std::string(to_string(episode.state))));
      item.set("revision", JsonValue(static_cast<std::int64_t>(episode.revision.value())));
      item.set("severity", JsonValue(std::string(to_string(episode.peak_severity))));
      item.set("confidence", JsonValue(static_cast<std::int64_t>(episode.confidence)));
      item.set("first_seen", JsonValue(episode.first_seen.to_iso8601()));
      item.set("last_seen", JsonValue(episode.last_seen.to_iso8601()));
      item.set("observations", JsonValue(static_cast<std::int64_t>(episode.observations)));
      item.set("history_truncated", JsonValue(episode.history_truncated));
      JsonValue::Array citations;
      for (const EvidenceId& citation : episode.citations) {
        citations.push_back(JsonValue(citation.str()));
      }
      item.set("citations", JsonValue(std::move(citations)));
      episodes.push_back(JsonValue(std::move(item)));
    }
    root.set("episodes", JsonValue(std::move(episodes)));
  }

  if (request.include_evidence) {
    EvidenceQuery query;
    query.max_records = request.max_evidence;
    query.window_start = Timestamp(std::numeric_limits<std::int64_t>::min());
    query.window_end = Timestamp(std::numeric_limits<std::int64_t>::max());
    const std::vector<EvidenceRecord> records = store_->query(query);
    JsonValue::Array evidence;
    for (const EvidenceRecord& record : records) {
      JsonObject item;
      item.set("id", JsonValue(record.id.str()));
      item.set("kind", JsonValue(std::string(to_string(record.kind))));
      item.set("role", JsonValue(std::string(to_string(role_of(record.kind)))));
      item.set("subject", JsonValue(record.subject.str()));
      item.set("source", JsonValue(record.provenance.source.str()));
      item.set("authority", JsonValue(std::string(to_string(record.provenance.authority))));
      item.set("generation", JsonValue(record.fence.gen.str()));
      item.set("incarnation", JsonValue(static_cast<std::int64_t>(record.fence.incarnation.value())));
      item.set("sequence", JsonValue(static_cast<std::int64_t>(record.fence.sequence.value())));
      item.set("observed_at", JsonValue(record.observed_at.to_iso8601()));
      item.set("received_at", JsonValue(record.received_at.to_iso8601()));
      item.set("clock", JsonValue(std::string(to_string(record.clock))));
      item.set("value", JsonValue(record.value.scalar));
      item.set("unit", JsonValue(std::string(to_string(record.value.unit))));
      item.set("semantics", JsonValue(std::string(to_string(record.value.semantics))));
      item.set("support", JsonValue(std::string(to_string(record.support))));
      item.set("completeness", JsonValue(std::string(to_string(record.completeness))));
      item.set("recovered_from_snapshot", JsonValue(record.recovered_from_snapshot));
      item.set("retired", JsonValue(record.retired));
      evidence.push_back(JsonValue(std::move(item)));
    }
    root.set("evidence", JsonValue(std::move(evidence)));
  }

  if (request.include_metrics) {
    const RuntimeMetrics snapshot = metrics();
    JsonObject metrics;
    metrics.set("ingested", JsonValue(static_cast<std::int64_t>(snapshot.ingested)));
    metrics.set("ingest_rejected_fence",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_fence)));
    metrics.set("ingest_rejected_stale",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_stale)));
    metrics.set("ingest_rejected_replay",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_replay)));
    metrics.set("ingest_rejected_authority",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_authority)));
    metrics.set("ingest_rejected_invalid",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_invalid)));
    metrics.set("ingest_rejected_unknown_subject",
                JsonValue(static_cast<std::int64_t>(snapshot.ingest_rejected_unknown_subject)));
    metrics.set("evidence_evicted", JsonValue(static_cast<std::int64_t>(snapshot.evidence_evicted)));
    metrics.set("classifications", JsonValue(static_cast<std::int64_t>(snapshot.classifications)));
    metrics.set("localizations", JsonValue(static_cast<std::int64_t>(snapshot.localizations)));
    metrics.set("episodes_opened", JsonValue(static_cast<std::int64_t>(snapshot.episodes_opened)));
    metrics.set("evidence_retained", JsonValue(static_cast<std::int64_t>(snapshot.evidence_retained)));
    metrics.set("episodes_tracked", JsonValue(static_cast<std::int64_t>(snapshot.episodes_tracked)));
    metrics.set("live_sources", JsonValue(static_cast<std::int64_t>(snapshot.live_sources)));
    root.set("metrics", JsonValue(std::move(metrics)));
  }

  return JsonValue(std::move(root)).dump(2);
}

Status Runtime::admit_topology(Topology topology) {
  {
    const std::lock_guard<std::mutex> lock(config_mutex_);
    topology_ = std::make_shared<const Topology>(std::move(topology));
  }
  return rebuild_causal_graph();
}

std::shared_ptr<const Topology> Runtime::current_topology() const {
  const std::lock_guard<std::mutex> lock(config_mutex_);
  return topology_;
}

Status Runtime::add_causal_edge(const CausalEdge& edge) {
  return graph_.add_edge(edge, config_.limits);
}

Status Runtime::rebuild_causal_graph() {
  CausalGraph graph;
  const Limits& limits = config_.limits;
  const std::shared_ptr<const Topology> topology = current_topology();
  const GenerationVector generation = topology->generation();

  // Structural edges are only created when an observation actually reports the structure. A
  // topology is a claim; without an advertisement, oper-state or capacity record there is no
  // evidence that the adjacency exists in the observed world.
  std::map<Name, std::vector<EvidenceId>> evidence_by_subject;
  {
    EvidenceQuery query;
    query.max_records = limits.max_window_records;
    query.window_start = Timestamp(std::numeric_limits<std::int64_t>::min());
    query.window_end = Timestamp(std::numeric_limits<std::int64_t>::max());
    const std::vector<EvidenceRecord> records = store_->query(query);
    for (const EvidenceRecord& record : records) {
      if (record.kind != EvidenceKind::kTopologyAdvertisement &&
          record.kind != EvidenceKind::kOperState && record.kind != EvidenceKind::kCapacityAdvertisement) {
        continue;
      }
      if (record.support == Support::kUnsupported) {
        continue;
      }
      auto& bucket = evidence_by_subject[Name::unchecked(record.subject.str())];
      if (bucket.size() < limits.max_citations) {
        bucket.push_back(record.id);
      }
    }
  }

  const auto citations_for = [&evidence_by_subject](const EvidenceSubject& subject) {
    const auto it = evidence_by_subject.find(Name::unchecked(subject.str()));
    if (it == evidence_by_subject.end()) {
      return std::vector<EvidenceId>{};
    }
    return it->second;
  };

  const auto link_edge = [&](const EvidenceSubject& from, const EvidenceSubject& to,
                             CausalEdgeKind kind, const char* rule,
                             std::uint32_t strength) -> Status {
    const std::vector<EvidenceId> citations = citations_for(from);
    if (citations.empty()) {
      return Status{};
    }
    CausalEdge edge;
    edge.from = from;
    edge.to = to;
    edge.kind = kind;
    edge.rule_id = rule;
    edge.citations = citations;
    edge.strength = strength;
    edge.generation = generation;
    return graph.add_edge(edge, limits);
  };

  for (const auto& entry : topology->paths()) {
    const EvidenceSubject path_subject(entry.second.id);
    for (const LinkId& hop : entry.second.hops) {
      const Status status =
          link_edge(path_subject, EvidenceSubject(hop), CausalEdgeKind::kPathMembership,
                    "topology-path-membership", 5);
      if (!status.ok()) {
        return status;
      }
    }
    // Consecutive hops on a path form an upstream -> downstream relation: a congested upstream hop
    // is a candidate explanation for the hop that follows it. The edge is only created when the
    // upstream hop was actually advertised, so it is evidence backed like every other edge.
    for (std::size_t hop = 0; hop + 1 < entry.second.hops.size(); ++hop) {
      const Status status = link_edge(EvidenceSubject(entry.second.hops[hop]),
                                      EvidenceSubject(entry.second.hops[hop + 1]),
                                      CausalEdgeKind::kPathMembership, "topology-path-upstream", 4);
      if (!status.ok()) {
        return status;
      }
    }
  }
  for (const auto& entry : topology->queues()) {
    const Status status = link_edge(EvidenceSubject(entry.second.id),
                                    EvidenceSubject(entry.second.link),
                                    CausalEdgeKind::kQueueOnLink, "topology-queue-on-link", 5);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& entry : topology->buffers()) {
    const Status status = link_edge(EvidenceSubject(entry.second.id),
                                    EvidenceSubject(entry.second.port),
                                    CausalEdgeKind::kBufferOnPort, "topology-buffer-on-port", 5);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& entry : topology->flows()) {
    const Status status = link_edge(EvidenceSubject(entry.second.id),
                                    EvidenceSubject(entry.second.path),
                                    CausalEdgeKind::kFlowOnPath, "topology-flow-on-path", 5);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& entry : topology->links()) {
    const EvidenceSubject link_subject(entry.second.id);
    const Status forward = link_edge(link_subject, EvidenceSubject(entry.second.endpoint_a),
                                     CausalEdgeKind::kTopologyAdjacency, "topology-link-endpoint", 3);
    if (!forward.ok()) {
      return forward;
    }
    const Status backward = link_edge(link_subject, EvidenceSubject(entry.second.endpoint_b),
                                      CausalEdgeKind::kTopologyAdjacency, "topology-link-endpoint",
                                      3);
    if (!backward.ok()) {
      return backward;
    }
  }

  {
    const std::lock_guard<std::mutex> lock(config_mutex_);
    graph_ = std::move(graph);
  }
  return Status{};
}

Result<SaveReport> Runtime::save() {
  if (snapshot_store_ == nullptr) {
    return make_error(ErrorCode::kUnsupported, "persistence is not configured");
  }
  const std::shared_ptr<const Topology> topology = current_topology();
  SnapshotContent content;
  content.metadata.policy_version = config_.classification.version;
  content.metadata.policy_digest = config_.classification.digest();
  content.metadata.limits_digest = config_.limits.digest();
  content.metadata.topology_digest = topology->digest();
  content.metadata.created_at = now_wall_clock();
  content.metadata.runtime_instance = runtime_instance_id();
  content.metadata.generation = topology->generation();
  content.metadata.producer_version = build_identity();
  const RuntimeMetrics snapshot = metrics();
  content.metadata.accepted_total = snapshot.ingested;
  content.metadata.rejected_total = snapshot.ingest_rejected_fence + snapshot.ingest_rejected_invalid;
  content.topology = *topology;
  content.episodes = registry_->all();

  EvidenceQuery query;
  query.max_records = config_.limits.max_snapshot_evidence;
  query.window_start = Timestamp(std::numeric_limits<std::int64_t>::min());
  query.window_end = Timestamp(std::numeric_limits<std::int64_t>::max());
  content.evidence = store_->query(query);
  content.evidence_truncated = store_->size() > content.evidence.size();

  auto report = snapshot_store_->save(content);
  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  if (report.ok()) {
    ++metrics_.snapshots_saved;
  } else {
    ++metrics_.snapshot_failures;
  }
  return report;
}

Result<LoadReport> Runtime::load() {
  if (snapshot_store_ == nullptr) {
    return make_error(ErrorCode::kUnsupported, "persistence is not configured");
  }
  auto report = snapshot_store_->load(config_.classification);
  if (!report.ok()) {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.snapshot_failures;
    return report.error();
  }
  LoadReport loaded = report.value();
  if (loaded.outcome == LoadOutcome::kLoadedNothing) {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.snapshot_failures;
    return loaded;
  }

  {
    const std::lock_guard<std::mutex> lock(config_mutex_);
    topology_ = std::make_shared<const Topology>(loaded.content.topology);
  }
  std::size_t stale_episodes = 0;
  for (const Episode& episode : loaded.content.episodes) {
    const Status restored = registry_->restore(episode);
    if (restored.ok()) {
      continue;
    }
    if (restored.code() == ErrorCode::kConflict) {
      // The live registry already holds a newer revision: a restore must never move state
      // backwards, so the older snapshot copy is skipped and reported.
      ++stale_episodes;
      continue;
    }
    return restored.error();
  }
  if (stale_episodes > 0) {
    loaded.notes.push_back(std::to_string(stale_episodes) +
                           " episode(s) in the snapshot were older than the live state and were "
                           "not applied");
  }
  for (const EvidenceRecord& record : loaded.content.evidence) {
    auto ingested = store_->ingest_recovered(record);
    if (!ingested.ok()) {
      return ingested.error();
    }
  }
  // Liveness is process local: after a restart no source is live until it is heard from again.
  store_->reset_liveness();
  const Status rebuilt = rebuild_causal_graph();
  if (!rebuilt.ok()) {
    return rebuilt.error();
  }

  const std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.snapshots_loaded;
  ++metrics_.liveness_resets;
  return loaded;
}

Status Runtime::start_workers() {
  if (config_.limits.worker_threads == 0) {
    return Status(make_error(ErrorCode::kUnsupported,
                             "worker_threads is zero: this runtime runs synchronously"));
  }
  if (workers_ == nullptr) {
    workers_ = std::make_unique<WorkerPool>();
  }
  return workers_->start(config_.limits.worker_threads, config_.limits.max_pending_tasks);
}

Status Runtime::stop_workers() {
  if (workers_ == nullptr) {
    return Status{};
  }
  workers_->stop();
  return Status{};
}

Status Runtime::request_maintenance() {
  if (workers_ == nullptr) {
    return Status(make_error(ErrorCode::kUnsupported, "worker pool is not running"));
  }
  return workers_->submit([this]() {
    // The task captures no lock and calls only component level operations, each of which takes
    // and releases its own lock. It never submits further work, so the pool cannot self deadlock.
    auto result = run_maintenance(now_wall_clock());
    (void)result;
  });
}

void Runtime::request_shutdown() {
  shutting_down_.store(true, std::memory_order_release);
  cancel_.cancel("runtime shutdown requested");
}

bool Runtime::shutting_down() const noexcept {
  return shutting_down_.load(std::memory_order_acquire);
}

Result<std::size_t> Runtime::run_maintenance(Timestamp now) {
  std::size_t changes = 0;
  auto advanced = registry_->advance(now, config_.episode, cancel_.token());
  if (!advanced.ok()) {
    return advanced.error();
  }
  changes += advanced.value().size();
  {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.maintenance_runs;
  }
  const Duration horizon = retention_horizon(config_.classification);
  auto swept = store_->sweep(Timestamp(now.unix_nanos() - horizon.nanos()), now,
                             config_.limits.max_window_records, cancel_.token());
  if (!swept.ok()) {
    return swept.error();
  }
  changes += swept.value();
  return changes;
}

RuntimeMetrics Runtime::metrics() const {
  // No component lock is taken while the metrics lock is held: each source of truth is read
  // first, then merged. This keeps the lock graph acyclic (see docs/concurrency.md).
  RuntimeMetrics snapshot;
  {
    const std::lock_guard<std::mutex> lock(metrics_mutex_);
    snapshot = metrics_;
  }
  const std::size_t retained = store_->size();
  const std::size_t tracked = registry_->size();
  const StoreStats store_stats = store_->stats();
  const bool workers_running = workers_ != nullptr && workers_->running();
  std::uint64_t completed = 0;
  std::uint64_t rejected = 0;
  std::uint64_t submitted = 0;
  if (workers_ != nullptr) {
    completed = workers_->completed.load(std::memory_order_relaxed);
    rejected = workers_->rejected.load(std::memory_order_relaxed);
    submitted = workers_->submitted.load(std::memory_order_relaxed);
  }
  snapshot.evidence_retained = retained;
  snapshot.episodes_tracked = tracked;
  snapshot.live_sources = store_stats.live_sources;
  snapshot.known_sources = store_stats.sources;
  snapshot.evidence_evicted = store_stats.evicted;
  snapshot.workers_running = workers_running;
  snapshot.tasks_completed = completed;
  snapshot.tasks_rejected = rejected;
  snapshot.tasks_submitted = submitted;
  snapshot.shutting_down = shutting_down();
  return snapshot;
}

}  // namespace congestion
