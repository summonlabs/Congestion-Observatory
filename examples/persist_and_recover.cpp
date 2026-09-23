// Congestion Observatory - persistence, restart and conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the restart contract: history survives, liveness does not, and damaged state is
// rejected rather than partially applied.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "congestion/congestion.hpp"

namespace {

constexpr std::int64_t kBaseSeconds = 1700000000;

congestion::Timestamp at(std::int64_t offset) {
  return congestion::Timestamp::from_unix_seconds(kBaseSeconds + offset).value();
}

congestion::Topology build_topology() {
  congestion::TopologyBuilder builder;
  builder.set_generation(congestion::GenerationVector{congestion::Epoch(1),
                                                     congestion::Generation(1),
                                                     congestion::Revision(1)});
  congestion::Node leaf;
  leaf.id = congestion::NodeId::unchecked("leaf");
  leaf.kind = congestion::NodeKind::kSwitch;
  (void)builder.add_node(leaf);
  congestion::Port port;
  port.id = congestion::PortId::unchecked("leaf/1");
  port.node = leaf.id;
  port.speed_bps = 100000000000ULL;
  (void)builder.add_port(port);
  congestion::Link link;
  link.id = congestion::LinkId::unchecked("uplink");
  link.endpoint_a = port.id;
  link.endpoint_b = port.id;
  (void)builder.add_link(link);
  auto topology = builder.build(congestion::Limits{});
  return topology.ok() ? topology.value() : congestion::Topology{};
}

congestion::EvidenceRecord drop_evidence(const congestion::GenerationVector& generation,
                                         std::uint64_t sequence, congestion::Timestamp observed) {
  congestion::EvidenceRecord record;
  record.kind = congestion::EvidenceKind::kDropCount;
  record.subject = congestion::EvidenceSubject(congestion::LinkId::unchecked("uplink"));
  record.provenance.source = congestion::SourceId::unchecked("collector");
  record.provenance.authority = congestion::AuthorityLevel::kMeasured;
  record.fence.source = record.provenance.source;
  record.fence.boot = congestion::BootId(1);
  record.fence.incarnation = congestion::Incarnation(1);
  record.fence.gen = generation;
  record.fence.sequence = congestion::Sequence(sequence);
  record.observed_at = observed;
  record.received_at = observed;
  record.clock = congestion::ClockDomain::kCollectorWallClock;
  record.value.scalar = 0.01;
  record.value.unit = congestion::ObservationUnit::kRatio;
  record.value.semantics = congestion::ValueSemantics::kRate;
  record.validity = congestion::Duration::from_seconds(60);
  record.id = congestion::compute_evidence_id(record);
  return record;
}

congestion::RuntimeConfig config_for(const congestion::Topology& topology,
                                     const std::string& directory) {
  congestion::RuntimeConfig config;
  config.topology = topology;
  config.state_directory = directory;
  config.state_basename = "observatory";
  return config;
}

}  // namespace

int main() {
  using namespace congestion;

  const Topology topology = build_topology();
  const EvidenceSubject link = EvidenceSubject(LinkId::unchecked("uplink"));
  const GenerationVector generation = topology.generation();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "congestion-observatory-example";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::string state = directory.string();

  EpisodeId episode_id;
  {
    auto runtime = Runtime::create(config_for(topology, state));
    if (!runtime.ok()) {
      std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
      return 1;
    }
    if (!runtime.value()->ingest(drop_evidence(generation, 1, at(0)), at(0)).ok()) {
      std::printf("ingest failed\n");
      return 1;
    }
    EpisodeUpdate update;
    if (!runtime.value()->evaluate(link, Duration::from_seconds(10), at(0), &update).ok()) {
      std::printf("evaluate failed\n");
      return 1;
    }
    episode_id = update.id;
    auto saved = runtime.value()->save();
    if (!saved.ok()) {
      std::printf("save failed: %s\n", saved.error().describe().c_str());
      return 1;
    }
    std::printf("saved %llu bytes to %s\n",
                static_cast<unsigned long long>(saved.value().bytes_written),
                saved.value().primary_path.c_str());
  }

  {
    auto runtime = Runtime::create(config_for(topology, state));
    if (!runtime.ok()) {
      std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
      return 1;
    }
    auto loaded = runtime.value()->load();
    if (!loaded.ok()) {
      std::printf("load failed: %s\n", loaded.error().describe().c_str());
      return 1;
    }
    std::printf("load outcome=%s episodes=%zu evidence=%zu live_sources=%zu\n",
                std::string(to_string(loaded.value().outcome)).c_str(),
                runtime.value()->metrics().episodes_tracked,
                runtime.value()->metrics().evidence_retained,
                runtime.value()->metrics().live_sources);

    auto episode = runtime.value()->episode(episode_id);
    if (!episode.ok()) {
      std::printf("episode was not preserved: %s\n", episode.error().describe().c_str());
      return 1;
    }
    auto assessment = runtime.value()->classify(link, Duration::from_seconds(3600), at(1));
    if (!assessment.ok()) {
      std::printf("classification failed: %s\n", assessment.error().describe().c_str());
      return 1;
    }
    std::printf("after restart: verdict=%s congestion=%s blockers=%zu\n",
                std::string(to_string(assessment.value().verdict)).c_str(),
                assessment.value().congestion_asserted ? "yes" : "no",
                assessment.value().blockers.size());

    const bool history_kept = episode.value().id == episode_id;
    const bool liveness_lost = runtime.value()->metrics().live_sources == 0;
    const bool not_reasserted = !assessment.value().congestion_asserted;
    if (!history_kept || !liveness_lost || !not_reasserted) {
      std::printf("example persist_and_recover: unexpected outcome\n");
      return 1;
    }
  }

  // Damage the primary snapshot: recovery falls back to the previous good one or reports that
  // nothing can be loaded. It never applies a partial snapshot.
  {
    const std::string primary = state + "/observatory.snapshot";
    std::fstream file(primary, std::ios::binary | std::ios::in | std::ios::out);
    if (file) {
      file.seekg(24, std::ios::beg);
      char byte = 0;
      file.read(&byte, 1);
      byte = static_cast<char>(byte ^ 0x5A);
      file.seekp(24, std::ios::beg);
      file.write(&byte, 1);
      file.close();
    }
    auto runtime = Runtime::create(config_for(topology, state));
    if (!runtime.ok()) {
      std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
      return 1;
    }
    auto loaded = runtime.value()->load();
    if (!loaded.ok()) {
      std::printf("load failed: %s\n", loaded.error().describe().c_str());
      return 1;
    }
    std::printf("with damaged primary: outcome=%s notes=%zu\n",
                std::string(to_string(loaded.value().outcome)).c_str(),
                loaded.value().notes.size());
  }

  std::filesystem::remove_all(directory, error);
  std::printf("example persist_and_recover: ok\n");
  return 0;
}
