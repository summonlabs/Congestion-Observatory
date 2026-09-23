// Congestion Observatory - command line inspection and ingestion tooling.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "co_support.hpp"
#include "congestion/congestion.hpp"

namespace {

using congestion::Duration;
using congestion::ErrorCode;
using congestion::Epoch;
using congestion::EvidenceRecord;
using congestion::EvidenceSubject;
using congestion::Generation;
using congestion::GenerationVector;
using congestion::Incarnation;
using congestion::IngestOutcome;
using congestion::Limits;
using congestion::Link;
using congestion::LinkId;
using congestion::Node;
using congestion::NodeId;
using congestion::NodeKind;
using congestion::Port;
using congestion::PortId;
using congestion::Result;
using congestion::Revision;
using congestion::Runtime;
using congestion::RuntimeConfig;
using congestion::Status;
using congestion::Timestamp;
using congestion::Topology;
using congestion::TopologyBuilder;

struct Options {
  std::map<std::string, std::string> values{};
  std::set<std::string> flags{};
  std::vector<std::string> positional{};

  [[nodiscard]] bool has(const std::string& key) const {
    return values.find(key) != values.end() || flags.find(key) != flags.end();
  }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  [[nodiscard]] std::int64_t get_int(const std::string& key, std::int64_t fallback) const {
    const auto it = values.find(key);
    if (it == values.end()) {
      return fallback;
    }
    try {
      return std::stoll(it->second);
    } catch (...) {
      return fallback;
    }
  }
  [[nodiscard]] bool get_bool(const std::string& key) const { return has(key); }
};

void print_usage() {
  std::cout <<
      "congestion-observatory - congestion evidence aggregation and causal localization\n"
      "\n"
      "usage: congestion-observatory <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                       print version and build identity\n"
      "  limits                        print the default resource bounds and their digest\n"
      "  topology   --file <json>      validate a topology document and print its digest\n"
      "  ingest     --file <json>      ingest an evidence document\n"
      "  classify   --subject <k:n>    classify a subject over a window\n"
      "  localize   --subject <k:n>    localize the cause of an asserted congestion\n"
      "  episodes                      list episodes\n"
      "  episode    --id <hex>         print one episode with its transitions\n"
      "  advance    --now <instant>    apply episode resolution and retention windows\n"
      "  explain    --subject|--id     explain a classification or an episode\n"
      "  export                        export the runtime state as JSON\n"
      "  save | load                   persist or restore the runtime state\n"
      "  serve      --port <n>         serve the ingestion protocol on loopback\n"
      "  push       --endpoint h:p     push an evidence document to a server\n"
      "  selfcheck                     run the built-in invariant checks\n"
      "  bench      --records <n>      measure completed ingestion and analysis work\n"
      "\n"
      "common options:\n"
      "  --state <dir>      snapshot directory (enables persistence)\n"
      "  --topology <json>  topology document to admit\n"
      "  --out <file>       write output to a file instead of stdout\n"
      "  --now <instant>    evaluation instant (ISO-8601 or @epoch-seconds)\n"
      "  --window <dur>     evaluation window (default 10s)\n"
      "  --json             machine readable output\n";
}

Result<Timestamp> resolve_now(const Options& options) {
  if (options.has("now")) {
    return congestion::tools::parse_timestamp(options.get("now", ""));
  }
  return congestion::now_wall_clock();
}

Result<Limits> resolve_limits(const Options& options) {
  Limits limits;
  if (options.get_bool("minimal-limits")) {
    limits = congestion::minimal_limits();
  }
  limits.worker_threads = static_cast<std::size_t>(options.get_int("workers", 0));
  const Status valid = limits.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  return limits;
}

struct RuntimeBundle {
  std::unique_ptr<Runtime> runtime{};
  bool persistence{false};
};

Result<std::unique_ptr<Runtime>> make_runtime(const Options& options, bool load_state,
                                              const std::vector<EvidenceRecord>* preload) {
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return limits.error();
  }
  RuntimeConfig config;
  config.limits = limits.value();
  if (options.has("topology")) {
    auto topology = congestion::tools::load_topology_file(options.get("topology", ""), config.limits);
    if (!topology.ok()) {
      return topology.error();
    }
    config.topology = std::move(topology.value());
  }
  if (options.has("state")) {
    config.state_directory = options.get("state", "");
    config.state_basename = options.get("state-name", "congestion-observatory");
  }
  const std::string policy_version = options.get("policy-version", config.classification.version);
  config.classification.version = policy_version;
  if (options.has("window")) {
    auto window = congestion::tools::parse_duration(options.get("window", "10s"));
    if (!window.ok()) {
      return window.error();
    }
    config.classification.evaluation_window = window.value();
  }
  config.start_workers = options.get_int("workers", 0) > 0;

  auto runtime = Runtime::create(std::move(config));
  if (!runtime.ok()) {
    return runtime.error();
  }
  if (load_state && runtime.value()->persistence_enabled()) {
    auto loaded = runtime.value()->load();
    if (!loaded.ok()) {
      return loaded.error();
    }
  }
  if (preload != nullptr) {
    const Timestamp now = congestion::now_wall_clock();
    for (const EvidenceRecord& record : *preload) {
      auto outcome = runtime.value()->ingest(record, now);
      if (!outcome.ok()) {
        return outcome.error();
      }
    }
    const Status rebuilt = runtime.value()->rebuild_causal_graph();
    if (!rebuilt.ok()) {
      return rebuilt.error();
    }
  }
  return runtime;
}

Result<EvidenceSubject> resolve_subject(const Options& options) {
  const std::string text = options.get("subject", "");
  if (text.empty()) {
    return congestion::make_error(ErrorCode::kInvalidArgument, "--subject is required");
  }
  return congestion::parse_subject(text);
}

Result<Duration> resolve_window(const Options& options) {
  return congestion::tools::parse_duration(options.get("window", "10s"));
}

int report_error(const congestion::Error& error) {
  std::cerr << "error: " << congestion::to_string(error.code()) << ": " << error.describe() << "\n";
  return 2;
}

int command_version() {
  std::cout << congestion::build_identity() << "\n";
  return 0;
}

int command_limits(const Options& options) {
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return report_error(limits.error());
  }
  if (options.has("json")) {
    congestion::JsonObject root;
    root.set("max_retained_evidence",
             congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_retained_evidence)));
    root.set("max_sources", congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_sources)));
    root.set("max_episodes", congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_episodes)));
    root.set("max_window_records",
             congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_window_records)));
    root.set("max_frame_bytes", congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_frame_bytes)));
    root.set("max_snapshot_bytes",
             congestion::JsonValue(static_cast<std::int64_t>(limits.value().max_snapshot_bytes)));
    root.set("digest", congestion::JsonValue(congestion::to_hex(limits.value().digest())));
    std::cout << congestion::JsonValue(std::move(root)).dump(2) << "\n";
    return 0;
  }
  std::cout << limits.value().describe() << "\n";
  std::cout << "digest=" << congestion::to_hex(limits.value().digest()) << "\n";
  return 0;
}

int command_topology(const Options& options) {
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return report_error(limits.error());
  }
  if (!options.has("file")) {
    return report_error(congestion::make_error(ErrorCode::kInvalidArgument, "--file is required"));
  }
  auto topology = congestion::tools::load_topology_file(options.get("file", ""), limits.value());
  if (!topology.ok()) {
    return report_error(topology.error());
  }
  std::cout << congestion::tools::describe_topology(topology.value());
  return 0;
}

int command_ingest(const Options& options) {
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return report_error(limits.error());
  }
  if (!options.has("file")) {
    return report_error(congestion::make_error(ErrorCode::kInvalidArgument, "--file is required"));
  }
  auto records = congestion::tools::load_evidence_file(options.get("file", ""), limits.value());
  if (!records.ok()) {
    return report_error(records.error());
  }
  // Ingestion accumulates: an existing snapshot is loaded first, the document is then applied
  // as live evidence, and --save persists the combined state.
  auto runtime = make_runtime(options, true, &records.value());
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  const auto metrics = runtime.value()->metrics();
  const std::int64_t accepted = static_cast<std::int64_t>(metrics.ingested);
  const std::int64_t rejected = static_cast<std::int64_t>(
      metrics.ingest_rejected_fence + metrics.ingest_rejected_invalid +
      metrics.ingest_rejected_stale + metrics.ingest_rejected_replay +
      metrics.ingest_rejected_authority + metrics.ingest_rejected_unknown_subject);
  const Status rebuilt = runtime.value()->rebuild_causal_graph();
  if (!rebuilt.ok()) {
    return report_error(rebuilt.error());
  }
  if (options.has("save")) {
    auto saved = runtime.value()->save();
    if (!saved.ok()) {
      return report_error(saved.error());
    }
  }
  std::cout << "accepted=" << accepted << " rejected=" << rejected << "\n";
  return rejected == 0 ? 0 : 3;
}

// Loads the persisted state, applies an optional evidence document as live observations, and
// returns the resulting runtime. Analysis commands use this so that a restart plus a fresh
// observation is a first class workflow rather than an implicit one.
Result<std::unique_ptr<Runtime>> make_analysis_runtime(const Options& options,
                                                       std::vector<EvidenceRecord>& scratch) {
  if (options.has("file")) {
    auto limits = resolve_limits(options);
    if (!limits.ok()) {
      return limits.error();
    }
    auto records = congestion::tools::load_evidence_file(options.get("file", ""), limits.value());
    if (!records.ok()) {
      return records.error();
    }
    scratch = std::move(records.value());
    return make_runtime(options, true, &scratch);
  }
  return make_runtime(options, true, nullptr);
}

int command_classify(const Options& options) {
  std::vector<EvidenceRecord> scratch;
  auto runtime = make_analysis_runtime(options, scratch);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto subject = resolve_subject(options);
  if (!subject.ok()) {
    return report_error(subject.error());
  }
  auto window = resolve_window(options);
  if (!window.ok()) {
    return report_error(window.error());
  }
  auto now = resolve_now(options);
  if (!now.ok()) {
    return report_error(now.error());
  }
  auto assessment = runtime.value()->classify(subject.value(), window.value(), now.value());
  if (!assessment.ok()) {
    return report_error(assessment.error());
  }
  if (options.has("json")) {
    congestion::JsonObject root;
    root.set("subject", congestion::JsonValue(assessment.value().subject.str()));
    root.set("verdict",
             congestion::JsonValue(std::string(congestion::to_string(assessment.value().verdict))));
    root.set("severity",
             congestion::JsonValue(std::string(congestion::to_string(assessment.value().severity))));
    root.set("confidence", congestion::JsonValue(static_cast<std::int64_t>(assessment.value().confidence)));
    root.set("congestion_asserted", congestion::JsonValue(assessment.value().congestion_asserted));
    root.set("mechanisms", congestion::JsonValue(congestion::describe_mechanisms(assessment.value().mechanisms)));
    root.set("feature_digest", congestion::JsonValue(congestion::to_hex(assessment.value().features.feature_digest)));
    congestion::JsonValue::Array blockers;
    for (const auto& blocker : assessment.value().blockers) {
      congestion::JsonObject item;
      item.set("code", congestion::JsonValue(blocker.code));
      item.set("detail", congestion::JsonValue(blocker.detail));
      blockers.push_back(congestion::JsonValue(std::move(item)));
    }
    root.set("blockers", congestion::JsonValue(std::move(blockers)));
    congestion::JsonValue::Array citations;
    for (const auto& citation : assessment.value().citations) {
      citations.push_back(congestion::JsonValue(citation.str()));
    }
    root.set("citations", congestion::JsonValue(std::move(citations)));
    std::cout << congestion::JsonValue(std::move(root)).dump(2) << "\n";
    return assessment.value().congestion_asserted ? 0 : 1;
  }
  std::cout << congestion::tools::describe_assessment(assessment.value());
  return assessment.value().congestion_asserted ? 0 : 1;
}

int command_localize(const Options& options) {
  std::vector<EvidenceRecord> scratch;
  auto runtime = make_analysis_runtime(options, scratch);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto subject = resolve_subject(options);
  if (!subject.ok()) {
    return report_error(subject.error());
  }
  auto window = resolve_window(options);
  if (!window.ok()) {
    return report_error(window.error());
  }
  auto now = resolve_now(options);
  if (!now.ok()) {
    return report_error(now.error());
  }
  auto result = runtime.value()->localize(subject.value(), window.value(), now.value());
  if (!result.ok()) {
    return report_error(result.error());
  }
  std::cout << "symptom=" << result.value().symptom.str()
            << " outcome=" << congestion::to_string(result.value().outcome)
            << " paths_examined=" << result.value().paths_examined
            << " depth_limited=" << (result.value().depth_limited ? "yes" : "no") << "\n";
  for (const auto& candidate : result.value().candidates) {
    std::cout << "  candidate " << candidate.subject.str() << " score=" << candidate.score
              << " confidence=" << candidate.confidence << " depth=" << candidate.depth << "\n";
    for (const std::string& basis : candidate.basis) {
      std::cout << "      " << basis << "\n";
    }
  }
  for (const auto& blocker : result.value().blockers) {
    std::cout << "  blocker " << blocker.code << ": " << blocker.detail << "\n";
  }
  return result.value().outcome == congestion::LocalizationOutcome::kUnlocalized ? 1 : 0;
}

int command_episodes(const Options& options) {
  auto runtime = make_runtime(options, true, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  congestion::EpisodeFilter filter;
  filter.include_closed = !options.get_bool("open-only");
  const auto episodes = runtime.value()->episodes(filter);
  if (options.has("json")) {
    congestion::JsonValue::Array items;
    for (const auto& episode : episodes) {
      congestion::JsonObject item;
      item.set("id", congestion::JsonValue(episode.id.str()));
      item.set("scope", congestion::JsonValue(episode.key.scope.str()));
      item.set("mechanism", congestion::JsonValue(std::string(congestion::to_string(episode.key.mechanism))));
      item.set("state", congestion::JsonValue(std::string(congestion::to_string(episode.state))));
      item.set("severity", congestion::JsonValue(std::string(congestion::to_string(episode.peak_severity))));
      item.set("revision", congestion::JsonValue(static_cast<std::int64_t>(episode.revision.value())));
      item.set("observations", congestion::JsonValue(static_cast<std::int64_t>(episode.observations)));
      items.push_back(congestion::JsonValue(std::move(item)));
    }
    std::cout << congestion::JsonValue(std::move(items)).dump(2) << "\n";
    return 0;
  }
  for (const auto& episode : episodes) {
    std::cout << episode.id.str() << " " << episode.key.scope.str() << " "
              << congestion::to_string(episode.key.mechanism) << " "
              << congestion::to_string(episode.state) << " severity="
              << congestion::to_string(episode.peak_severity)
              << " observations=" << episode.observations
              << " revision=" << episode.revision.value()
              << (episode.history_truncated ? " history_truncated" : "") << "\n";
  }
  std::cout << "episodes=" << episodes.size() << "\n";
  return 0;
}

int command_episode(const Options& options) {
  auto runtime = make_runtime(options, true, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto id = congestion::EpisodeId::parse(options.get("id", ""));
  if (!id.ok()) {
    return report_error(id.error());
  }
  auto episode = runtime.value()->episode(id.value());
  if (!episode.ok()) {
    return report_error(episode.error());
  }
  std::cout << "id=" << episode.value().id.str() << "\n"
            << "key=" << episode.value().key.str() << "\n"
            << "state=" << congestion::to_string(episode.value().state) << "\n"
            << "revision=" << episode.value().revision.value() << "\n"
            << "first_seen=" << episode.value().first_seen.to_iso8601() << "\n"
            << "last_seen=" << episode.value().last_seen.to_iso8601() << "\n"
            << "peak_severity=" << congestion::to_string(episode.value().peak_severity) << "\n";
  for (const auto& transition : episode.value().transitions) {
    std::cout << "  " << transition.at.to_iso8601() << " r" << transition.revision.value() << " "
              << congestion::to_string(transition.kind) << " " << transition.cause << "\n";
  }
  return 0;
}

int command_advance(const Options& options) {
  auto runtime = make_runtime(options, true, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto now = resolve_now(options);
  if (!now.ok()) {
    return report_error(now.error());
  }
  auto changes = runtime.value()->advance_episodes(now.value());
  if (!changes.ok()) {
    return report_error(changes.error());
  }
  for (const auto& update : changes.value()) {
    std::cout << update.id.str() << " " << congestion::to_string(update.kind) << " -> "
              << congestion::to_string(update.state) << "\n";
  }
  std::cout << "changes=" << changes.value().size() << "\n";
  if (options.has("save")) {
    auto saved = runtime.value()->save();
    if (!saved.ok()) {
      return report_error(saved.error());
    }
  }
  return 0;
}

int command_explain(const Options& options) {
  std::vector<EvidenceRecord> scratch;
  auto runtime = make_analysis_runtime(options, scratch);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  if (options.has("id")) {
    auto id = congestion::EpisodeId::parse(options.get("id", ""));
    if (!id.ok()) {
      return report_error(id.error());
    }
    auto explanation = runtime.value()->explain_episode(id.value());
    if (!explanation.ok()) {
      return report_error(explanation.error());
    }
    std::cout << explanation.value().render();
    return 0;
  }
  auto subject = resolve_subject(options);
  if (!subject.ok()) {
    return report_error(subject.error());
  }
  auto window = resolve_window(options);
  if (!window.ok()) {
    return report_error(window.error());
  }
  auto now = resolve_now(options);
  if (!now.ok()) {
    return report_error(now.error());
  }
  auto explanation = runtime.value()->explain(subject.value(), window.value(), now.value());
  if (!explanation.ok()) {
    return report_error(explanation.error());
  }
  std::cout << explanation.value().render();
  return 0;
}

int command_export(const Options& options) {
  auto runtime = make_runtime(options, true, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto now = resolve_now(options);
  if (!now.ok()) {
    return report_error(now.error());
  }
  congestion::ExportRequest request;
  request.include_evidence = options.get_bool("evidence");
  request.max_evidence = static_cast<std::size_t>(options.get_int("max-evidence", 256));
  request.max_episodes = static_cast<std::size_t>(options.get_int("max-episodes", 256));
  auto document = runtime.value()->export_json(request, now.value());
  if (!document.ok()) {
    return report_error(document.error());
  }
  std::cout << document.value() << "\n";
  return 0;
}

int command_save(const Options& options) {
  if (!options.has("state")) {
    return report_error(congestion::make_error(ErrorCode::kInvalidArgument,
                                               "--state is required for save"));
  }
  std::vector<EvidenceRecord> records;
  auto runtime = make_analysis_runtime(options, records);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto saved = runtime.value()->save();
  if (!saved.ok()) {
    return report_error(saved.error());
  }
  std::cout << "saved " << saved.value().bytes_written << " bytes to "
            << saved.value().primary_path << " crc=" << congestion::to_hex(saved.value().crc64)
            << "\n";
  return 0;
}

int command_load(const Options& options) {
  if (!options.has("state")) {
    return report_error(congestion::make_error(ErrorCode::kInvalidArgument,
                                               "--state is required for load"));
  }
  auto runtime = make_runtime(options, false, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  auto loaded = runtime.value()->load();
  if (!loaded.ok()) {
    return report_error(loaded.error());
  }
  const auto metrics = runtime.value()->metrics();
  std::cout << "outcome=" << congestion::to_string(loaded.value().outcome)
            << " episodes=" << metrics.episodes_tracked
            << " evidence=" << metrics.evidence_retained
            << " live_sources=" << metrics.live_sources << "\n";
  for (const std::string& note : loaded.value().notes) {
    std::cout << "  note: " << note << "\n";
  }
  return loaded.value().outcome == congestion::LoadOutcome::kLoadedNothing ? 1 : 0;
}

int command_serve(const Options& options) {
  auto runtime = make_runtime(options, true, nullptr);
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  congestion::ServerConfig config;
  config.port = static_cast<std::uint16_t>(options.get_int("port", 0));
  if (options.has("io-deadline")) {
    auto deadline = congestion::tools::parse_duration(options.get("io-deadline", "5s"));
    if (!deadline.ok()) {
      return report_error(deadline.error());
    }
    config.socket.io_deadline = deadline.value();
  }
  congestion::IngestServer server(*runtime.value(), config);
  auto bound = server.start();
  if (!bound.ok()) {
    return report_error(bound.error());
  }
  std::cout << "listening port=" << bound.value() << "\n" << std::flush;
  const Status served = server.run(runtime.value()->cancellation_token());
  if (!served.ok()) {
    return report_error(served.error());
  }
  if (options.has("save")) {
    auto saved = runtime.value()->save();
    if (!saved.ok()) {
      return report_error(saved.error());
    }
  }
  return 0;
}

int command_push(const Options& options) {
  const std::string endpoint = options.get("endpoint", "");
  const std::size_t separator = endpoint.find(':');
  if (separator == std::string::npos) {
    return report_error(congestion::make_error(ErrorCode::kInvalidArgument,
                                               "--endpoint must be host:port"));
  }
  congestion::IngestClientConfig config;
  config.host = endpoint.substr(0, separator);
  config.port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(separator + 1)));
  if (options.has("io-deadline")) {
    auto deadline = congestion::tools::parse_duration(options.get("io-deadline", "5s"));
    if (!deadline.ok()) {
      return report_error(deadline.error());
    }
    config.socket.io_deadline = deadline.value();
  }
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return report_error(limits.error());
  }
  config.limits = limits.value();

  std::vector<EvidenceRecord> records;
  if (options.has("file")) {
    auto loaded = congestion::tools::load_evidence_file(options.get("file", ""), config.limits);
    if (!loaded.ok()) {
      return report_error(loaded.error());
    }
    records = std::move(loaded.value());
  }
  auto result = congestion::push_records(config, records);
  if (!result.ok()) {
    return report_error(result.error());
  }
  std::cout << "sent=" << result.value().records_sent
            << " accepted=" << result.value().records_accepted
            << " rejected=" << result.value().records_rejected
            << " server=" << result.value().server_version << "\n";
  for (const auto& outcome : result.value().outcomes) {
    if (!outcome.stored) {
      std::cout << "  fenced " << congestion::to_string(outcome.fence) << ": " << outcome.detail
                << "\n";
    }
  }
  return result.value().records_rejected == 0 ? 0 : 3;
}

int command_selfcheck() {
  Limits limits;
  int failures = 0;
  const auto check = [&failures](const char* name, bool condition) {
    std::cout << (condition ? "ok   " : "FAIL ") << name << "\n";
    if (!condition) {
      ++failures;
    }
  };

  check("default limits are valid", limits.validate().ok());
  check("minimal limits are valid", congestion::minimal_limits().validate().ok());
  check("limits digest is stable", limits.digest() == Limits{}.digest());
  check("name validation rejects separators", !congestion::Name::parse("/bad").ok());
  check("name validation accepts canonical names", congestion::Name::parse("leaf1/port-1").ok());
  check("unchecked names reported no failures", congestion::unchecked_name_failure_count() == 0);

  EvidenceRecord record;
  record.kind = congestion::EvidenceKind::kLinkUtilization;
  record.subject = EvidenceSubject(congestion::LinkId::unchecked("leaf1-leaf2"));
  record.provenance.source = congestion::SourceId::unchecked("collector-1");
  record.fence.source = record.provenance.source;
  record.fence.incarnation = congestion::Incarnation(1);
  record.fence.gen.epoch = congestion::Epoch(1);
  record.fence.gen.generation = congestion::Generation(1);
  record.fence.gen.revision = congestion::Revision(1);
  record.fence.sequence = congestion::Sequence(1);
  record.observed_at = congestion::Timestamp::from_unix_seconds(1000).value();
  record.received_at = record.observed_at;
  record.clock = congestion::ClockDomain::kCollectorWallClock;
  record.value.scalar = 0.95;
  record.value.unit = congestion::ObservationUnit::kRatio;
  record.value.semantics = congestion::ValueSemantics::kGauge;
  const auto first = congestion::compute_evidence_id(record);
  EvidenceRecord copy = record;
  check("evidence identity is deterministic", first == congestion::compute_evidence_id(copy));

  const auto freshness = congestion::assess_freshness(
      record, congestion::Timestamp::from_unix_seconds(1000).value(),
      congestion::FreshnessPolicy{});
  check("fresh evidence is fresh", freshness.freshness == congestion::Freshness::kFresh);
  const auto stale = congestion::assess_freshness(
      record, congestion::Timestamp::from_unix_seconds(1200).value(),
      congestion::FreshnessPolicy{});
  check("old evidence is not fresh", stale.freshness != congestion::Freshness::kFresh);
  EvidenceRecord recovered = record;
  recovered.recovered_from_snapshot = true;
  const auto recovered_freshness = congestion::assess_freshness(
      recovered, congestion::Timestamp::from_unix_seconds(1000).value(),
      congestion::FreshnessPolicy{});
  check("recovered evidence is never fresh",
        recovered_freshness.freshness == congestion::Freshness::kUnknown);

  congestion::CausalGraph graph;
  congestion::CausalEdge edge;
  edge.from = EvidenceSubject(congestion::LinkId::unchecked("a"));
  edge.to = EvidenceSubject(congestion::LinkId::unchecked("b"));
  edge.rule_id = "selfcheck";
  const Status without_citations = graph.add_edge(edge, limits);
  check("causal edges require evidence",
        !without_citations.ok() &&
            without_citations.code() == ErrorCode::kPreconditionFailed);
  edge.citations.push_back(first);
  check("causal edges with evidence are accepted", graph.add_edge(edge, limits).ok());

  std::cout << (failures == 0 ? "selfcheck: all invariants hold\n"
                              : "selfcheck: failures detected\n");
  return failures == 0 ? 0 : 3;
}

// A minimal two switch fabric used by the benchmark command.
Result<Topology> bench_topology(const Limits& limits) {
  TopologyBuilder builder;
  builder.set_limits(limits);
  GenerationVector generation;
  generation.epoch = Epoch(1);
  generation.generation = Generation(1);
  generation.revision = Revision(1);
  builder.set_generation(generation);
  for (const char* name : {"leaf1", "leaf2"}) {
    Node node;
    node.id = NodeId::unchecked(name);
    node.kind = NodeKind::kSwitch;
    const Status added = builder.add_node(node);
    if (!added.ok()) {
      return added.error();
    }
  }
  struct PortSpec { const char* id; const char* node; };
  const PortSpec ports[] = {{"leaf1/1", "leaf1"}, {"leaf2/1", "leaf2"}};
  for (const PortSpec& spec : ports) {
    Port port;
    port.id = PortId::unchecked(spec.id);
    port.node = NodeId::unchecked(spec.node);
    port.speed_bps = 100000000000ULL;
    const Status added = builder.add_port(port);
    if (!added.ok()) {
      return added.error();
    }
  }
  Link link;
  link.id = LinkId::unchecked("leaf1-leaf2");
  link.endpoint_a = PortId::unchecked("leaf1/1");
  link.endpoint_b = PortId::unchecked("leaf2/1");
  link.capacity_bps = 100000000000ULL;
  const Status added = builder.add_link(link);
  if (!added.ok()) {
    return added.error();
  }
  return builder.build(limits);
}

int command_bench(const Options& options) {
  auto limits = resolve_limits(options);
  if (!limits.ok()) {
    return report_error(limits.error());
  }
  limits.value().worker_threads = 0;
  auto topology = bench_topology(limits.value());
  if (!topology.ok()) {
    return report_error(topology.error());
  }
  RuntimeConfig config;
  config.limits = limits.value();
  config.topology = topology.value();
  auto runtime = Runtime::create(std::move(config));
  if (!runtime.ok()) {
    return report_error(runtime.error());
  }
  const std::int64_t count = options.get_int("records", 2000);
  const Timestamp base = congestion::Timestamp::from_unix_seconds(1000).value();
  const auto start = congestion::monotonic_nanos();
  for (std::int64_t i = 0; i < count; ++i) {
    EvidenceRecord record;
    record.kind = (i % 3 == 0) ? congestion::EvidenceKind::kLinkUtilization
                               : (i % 3 == 1 ? congestion::EvidenceKind::kDropCount
                                             : congestion::EvidenceKind::kQueueOccupancy);
    record.subject = EvidenceSubject(congestion::LinkId::unchecked("leaf1-leaf2"));
    record.provenance.source = congestion::SourceId::unchecked("bench-source");
    record.fence.source = record.provenance.source;
    record.fence.incarnation = congestion::Incarnation(1);
    record.fence.gen.epoch = congestion::Epoch(1);
    record.fence.gen.generation = congestion::Generation(1);
    record.fence.gen.revision = congestion::Revision(1);
    record.fence.sequence = congestion::Sequence(static_cast<std::uint64_t>(i) + 1);
    record.observed_at = base;
    record.received_at = base;
    record.clock = congestion::ClockDomain::kCollectorWallClock;
    record.value.scalar = 0.5;
    record.value.unit = congestion::ObservationUnit::kRatio;
    record.value.semantics = congestion::ValueSemantics::kGauge;
    auto outcome = runtime.value()->ingest(record, base);
    if (!outcome.ok()) {
      return report_error(outcome.error());
    }
  }
  const auto ingest_done = congestion::monotonic_nanos();
  std::size_t classified = 0;
  const auto classify_start = congestion::monotonic_nanos();
  for (std::int64_t i = 0; i < count / 10 + 1; ++i) {
    auto assessment = runtime.value()->classify(
        EvidenceSubject(congestion::LinkId::unchecked("leaf1-leaf2")),
        Duration::from_seconds(10), base);
    if (!assessment.ok()) {
      return report_error(assessment.error());
    }
    ++classified;
  }
  const auto classify_done = congestion::monotonic_nanos();
  const auto ingest_nanos = ingest_done - start;
  const auto classify_nanos = classify_done - classify_start;
  const auto metrics = runtime.value()->metrics();
  std::printf("ingested=%llu retained=%zu\n", static_cast<unsigned long long>(metrics.ingested),
              metrics.evidence_retained);
  std::printf("classifications=%zu\n", classified);
  std::printf("ingest_ns=%lld (%.1f ns/record)\n", static_cast<long long>(ingest_nanos),
              static_cast<double>(ingest_nanos) / static_cast<double>(count));
  std::printf("classify_ns=%lld (%.1f ns/classification)\n", static_cast<long long>(classify_nanos),
              static_cast<double>(classify_nanos) / static_cast<double>(classified));
  return metrics.ingested == static_cast<std::uint64_t>(count) ? 0 : 3;
}

Options parse_options(int argc, char** argv, std::string& command) {
  Options options;
  int index = 1;
  if (index < argc) {
    command = argv[index];
    ++index;
  }
  while (index < argc) {
    std::string token = argv[index];
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        options.values[token.substr(2, equals - 2)] = token.substr(equals + 1);
        ++index;
        continue;
      }
      const std::string key = token.substr(2);
      if (index + 1 < argc && std::strncmp(argv[index + 1], "--", 2) != 0) {
        options.values[key] = argv[index + 1];
        index += 2;
        continue;
      }
      options.flags.insert(key);
      ++index;
      continue;
    }
    options.positional.push_back(token);
    ++index;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::string command;
  const Options options = parse_options(argc, argv, command);
  // --out redirects the process output to a file. Callers that drive the tool from another
  // program therefore never depend on shell redirection or on shell quoting rules.
  if (options.has("out")) {
    const std::string path = options.get("out", "");
    std::FILE* redirected = nullptr;
#if defined(_WIN32)
    const errno_t result = freopen_s(&redirected, path.c_str(), "wb", stdout);
    if (path.empty() || result != 0 || redirected == nullptr) {
#else
    redirected = std::freopen(path.c_str(), "wb", stdout);
    if (path.empty() || redirected == nullptr) {
#endif
      std::cerr << "error: cannot write to --out " << path << "\n";
      return 2;
    }
  }
  if (command.empty() || command == "help" || command == "--help") {
    print_usage();
    return command.empty() ? 1 : 0;
  }
  if (command == "version") return command_version();
  if (command == "limits") return command_limits(options);
  if (command == "topology") return command_topology(options);
  if (command == "ingest") return command_ingest(options);
  if (command == "classify") return command_classify(options);
  if (command == "localize") return command_localize(options);
  if (command == "episodes") return command_episodes(options);
  if (command == "episode") return command_episode(options);
  if (command == "advance") return command_advance(options);
  if (command == "explain") return command_explain(options);
  if (command == "export") return command_export(options);
  if (command == "save") return command_save(options);
  if (command == "load") return command_load(options);
  if (command == "serve") return command_serve(options);
  if (command == "push") return command_push(options);
  if (command == "selfcheck") return command_selfcheck();
  if (command == "bench") return command_bench(options);
  std::cerr << "unknown command: " << command << "\n";
  print_usage();
  return 1;
}
