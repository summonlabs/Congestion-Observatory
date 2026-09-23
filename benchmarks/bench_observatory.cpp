// Congestion Observatory - benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Each benchmark reports completed work: the number of records that were actually ingested,
// classified, localized and persisted, verified against the runtime's own counters. A benchmark
// that silently drops work is a failed benchmark, and exits non-zero.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "congestion/congestion.hpp"

namespace {

constexpr std::int64_t kBaseSeconds = 1700000000;

congestion::Timestamp at(std::int64_t offset) {
  return congestion::Timestamp::from_unix_seconds(kBaseSeconds + offset).value();
}

congestion::Topology build_topology(std::size_t links) {
  congestion::TopologyBuilder builder;
  builder.set_generation(congestion::GenerationVector{congestion::Epoch(1),
                                                     congestion::Generation(1),
                                                     congestion::Revision(1)});
  for (std::size_t i = 0; i < links; ++i) {
    const std::string index = std::to_string(i);
    congestion::Node node;
    node.id = congestion::NodeId::unchecked("leaf-" + index);
    node.kind = congestion::NodeKind::kSwitch;
    (void)builder.add_node(node);
    congestion::Port port;
    port.id = congestion::PortId::unchecked("leaf-" + index + "/1");
    port.node = node.id;
    port.speed_bps = 100000000000ULL;
    (void)builder.add_port(port);
    if (i == 0) {
      continue;
    }
    congestion::Link link;
    link.id = congestion::LinkId::unchecked("leaf-" + std::to_string(i - 1) + "-leaf-" + index);
    link.endpoint_a = congestion::PortId::unchecked("leaf-" + std::to_string(i - 1) + "/1");
    link.endpoint_b = port.id;
    link.capacity_bps = 100000000000ULL;
    (void)builder.add_link(link);
  }
  auto topology = builder.build(congestion::Limits{});
  return topology.ok() ? topology.value() : congestion::Topology{};
}

double nanos_between(std::chrono::steady_clock::time_point start,
                     std::chrono::steady_clock::time_point end) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

}  // namespace

int main(int argc, char** argv) {
  using namespace congestion;

  std::size_t records = 20000;
  std::size_t link_count = 16;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--records") {
      records = static_cast<std::size_t>(std::stoull(argv[i + 1]));
    } else if (flag == "--links") {
      link_count = static_cast<std::size_t>(std::stoull(argv[i + 1]));
    }
  }

  const Topology topology = build_topology(link_count);
  if (topology.links().empty()) {
    std::printf("benchmark setup failed\n");
    return 2;
  }
  RuntimeConfig config;
  config.topology = topology;
  config.limits.max_retained_evidence = records + 1024;
  config.limits.max_evidence_per_subject = records + 1024;
  config.limits.max_window_records = records + 1024;
  config.limits.max_snapshot_evidence = records + 1024;
  // The persistence scratch directory, and the persistent configuration, are prepared before the
  // first config is moved into its runtime.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "congestion-observatory-benchmark";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  RuntimeConfig persistent = config;
  persistent.state_directory = directory.string();
  persistent.state_basename = "benchmark";

  auto runtime = Runtime::create(std::move(config));
  if (!runtime.ok()) {
    std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
    return 2;
  }

  std::vector<LinkId> links;
  for (const auto& entry : topology.links()) {
    links.push_back(entry.second.id);
  }

  const SourceId source = SourceId::unchecked("bench-collector");
  const GenerationVector generation = topology.generation();

  // ---- ingestion ------------------------------------------------------------
  std::uint64_t sequence = 0;
  std::uint64_t ingested = 0;
  const auto ingest_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < records; ++i) {
    const LinkId& link = links[i % links.size()];
    EvidenceRecord record;
    record.kind = (i % 4 == 0)   ? EvidenceKind::kLinkUtilization
                  : (i % 4 == 1) ? EvidenceKind::kDropCount
                  : (i % 4 == 2) ? EvidenceKind::kQueueOccupancy
                                 : EvidenceKind::kOfferedDemand;
    record.subject = EvidenceSubject(link);
    record.provenance.source = source;
    record.provenance.authority = AuthorityLevel::kMeasured;
    record.fence.source = source;
    record.fence.boot = BootId(1);
    record.fence.incarnation = Incarnation(1);
    record.fence.gen = generation;
    record.fence.sequence = Sequence(++sequence);
    record.observed_at = at(0);
    record.received_at = at(0);
    record.clock = ClockDomain::kCollectorWallClock;
    record.value.scalar = (record.kind == EvidenceKind::kOfferedDemand) ? 50000000000.0 : 0.5;
    record.value.unit = (record.kind == EvidenceKind::kOfferedDemand)
                            ? ObservationUnit::kBitsPerSecond
                            : ObservationUnit::kRatio;
    record.value.semantics = ValueSemantics::kGauge;
    record.id = compute_evidence_id(record);
    auto outcome = runtime.value()->ingest(record, at(0));
    if (!outcome.ok() || !outcome.value().stored) {
      std::printf("ingest failed at record %zu\n", i);
      return 1;
    }
    ++ingested;
  }
  const auto ingest_end = std::chrono::steady_clock::now();

  // ---- classification -------------------------------------------------------
  std::uint64_t classified = 0;
  const std::size_t classification_rounds = records / 50 + 1;
  const auto classify_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < classification_rounds; ++i) {
    auto assessment = runtime.value()->classify(EvidenceSubject(links[i % links.size()]),
                                                Duration::from_seconds(10), at(1));
    if (!assessment.ok()) {
      std::printf("classification failed\n");
      return 1;
    }
    ++classified;
  }
  const auto classify_end = std::chrono::steady_clock::now();

  // ---- persistence ----------------------------------------------------------
  auto store_runtime = Runtime::create(std::move(persistent));
  if (!store_runtime.ok()) {
    std::printf("persistent runtime creation failed: %s\n",
                store_runtime.error().describe().c_str());
    return 2;
  }
  for (const EvidenceRecord& record :
       runtime.value()->evidence().query(EvidenceQuery{nullptr, nullptr, at(-1), at(0),
                                                       records, true, true, false})) {
    auto outcome = store_runtime.value()->ingest(record, at(0));
    if (!outcome.ok() || !outcome.value().stored) {
      std::printf("persistence preload failed\n");
      return 1;
    }
  }
  const auto save_start = std::chrono::steady_clock::now();
  auto saved = store_runtime.value()->save();
  const auto save_end = std::chrono::steady_clock::now();
  if (!saved.ok()) {
    std::printf("save failed: %s\n", saved.error().describe().c_str());
    return 1;
  }
  const auto load_start = std::chrono::steady_clock::now();
  auto loaded = store_runtime.value()->load();
  const auto load_end = std::chrono::steady_clock::now();
  if (!loaded.ok()) {
    std::printf("load failed: %s\n", loaded.error().describe().c_str());
    return 1;
  }

  const RuntimeMetrics metrics = runtime.value()->metrics();
  const double ingest_nanos = nanos_between(ingest_start, ingest_end);
  const double classify_nanos = nanos_between(classify_start, classify_end);
  const double save_nanos = nanos_between(save_start, save_end);
  const double load_nanos = nanos_between(load_start, load_end);

  std::printf("links=%zu records=%llu window_records_budget=%zu\n", links.size(),
              static_cast<unsigned long long>(ingested),
              runtime.value()->limits().max_window_records);
  std::printf("ingest_count=%llu retained=%zu ingest_ns=%.0f ns_per_record=%.1f records_per_s=%.0f\n",
              static_cast<unsigned long long>(ingested), metrics.evidence_retained, ingest_nanos,
              ingest_nanos / static_cast<double>(ingested),
              static_cast<double>(ingested) * 1e9 / ingest_nanos);
  std::printf("classifications=%llu classify_ns=%.0f ns_per_classification=%.1f per_s=%.0f\n",
              static_cast<unsigned long long>(classified), classify_nanos,
              classify_nanos / static_cast<double>(classified),
              static_cast<double>(classified) * 1e9 / classify_nanos);
  std::printf("snapshot_bytes=%llu save_ns=%.0f load_ns=%.0f\n",
              static_cast<unsigned long long>(saved.value().bytes_written), save_nanos, load_nanos);

  std::filesystem::remove_all(directory, error);

  const bool complete = metrics.ingested == ingested && classified == classification_rounds &&
                        loaded.value().outcome != LoadOutcome::kLoadedNothing;
  std::printf("benchmark completed_work=%s\n", complete ? "verified" : "INCOMPLETE");
  return complete ? 0 : 1;
}
