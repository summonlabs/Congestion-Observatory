// Congestion Observatory - minimal end to end pipeline.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the shortest path from raw observations to an explained verdict:
//   build a topology, ingest utilization and drop counters, classify, and print the reasoning.
//
// The example is self contained and deterministic: no wall clock, no network, no files.

#include <cstdio>
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
  congestion::Node leaf_a;
  leaf_a.id = congestion::NodeId::unchecked("leaf-a");
  leaf_a.kind = congestion::NodeKind::kSwitch;
  congestion::Node leaf_b = leaf_a;
  leaf_b.id = congestion::NodeId::unchecked("leaf-b");
  (void)builder.add_node(leaf_a);
  (void)builder.add_node(leaf_b);
  congestion::Port port_a;
  port_a.id = congestion::PortId::unchecked("leaf-a/1");
  port_a.node = leaf_a.id;
  port_a.speed_bps = 100000000000ULL;
  congestion::Port port_b = port_a;
  port_b.id = congestion::PortId::unchecked("leaf-b/1");
  port_b.node = leaf_b.id;
  (void)builder.add_port(port_a);
  (void)builder.add_port(port_b);
  congestion::Link link;
  link.id = congestion::LinkId::unchecked("leaf-a-leaf-b");
  link.endpoint_a = port_a.id;
  link.endpoint_b = port_b.id;
  link.capacity_bps = 100000000000ULL;
  (void)builder.add_link(link);
  auto topology = builder.build(congestion::Limits{});
  return topology.ok() ? topology.value() : congestion::Topology{};
}

congestion::EvidenceRecord make_record(const congestion::SourceId& source,
                                       const congestion::GenerationVector& generation,
                                       std::uint64_t sequence, congestion::EvidenceKind kind,
                                       const congestion::EvidenceSubject& subject, double value,
                                       congestion::ObservationUnit unit,
                                       congestion::ValueSemantics semantics,
                                       std::int64_t offset) {
  congestion::EvidenceRecord record;
  record.kind = kind;
  record.subject = subject;
  record.provenance.source = source;
  record.provenance.authority = congestion::AuthorityLevel::kMeasured;
  record.provenance.transport = "example";
  record.provenance.collector = "example";
  record.fence.source = source;
  record.fence.boot = congestion::BootId(1);
  record.fence.incarnation = congestion::Incarnation(1);
  record.fence.gen = generation;
  record.fence.sequence = congestion::Sequence(sequence);
  record.observed_at = at(offset);
  record.received_at = at(offset);
  record.clock = congestion::ClockDomain::kCollectorWallClock;
  record.value.scalar = value;
  record.value.unit = unit;
  record.value.semantics = semantics;
  record.validity = congestion::Duration::from_seconds(60);
  record.id = congestion::compute_evidence_id(record);
  return record;
}

}  // namespace

int main() {
  using namespace congestion;

  const Topology topology = build_topology();
  auto runtime = Runtime::create([&]() {
    RuntimeConfig config;
    config.topology = topology;
    return config;
  }());
  if (!runtime.ok()) {
    std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
    return 1;
  }

  const SourceId source = SourceId::unchecked("example-collector");
  const GenerationVector generation = topology.generation();
  const EvidenceSubject link = EvidenceSubject(LinkId::unchecked("leaf-a-leaf-b"));

  std::uint64_t sequence = 0;
  std::vector<EvidenceRecord> observations;
  observations.push_back(make_record(source, generation, ++sequence, EvidenceKind::kLinkUtilization,
                                     link, 0.98, ObservationUnit::kRatio,
                                     ValueSemantics::kGauge, 0));
  observations.push_back(make_record(source, generation, ++sequence,
                                     EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                                     ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge, 0));
  observations.push_back(make_record(source, generation, ++sequence, EvidenceKind::kOfferedDemand,
                                     link, 97000000000.0, ObservationUnit::kBitsPerSecond,
                                     ValueSemantics::kGauge, 0));

  for (const EvidenceRecord& record : observations) {
    auto outcome = runtime.value()->ingest(record, at(0));
    if (!outcome.ok()) {
      std::printf("ingest failed: %s\n", outcome.error().describe().c_str());
      return 1;
    }
  }

  auto first = runtime.value()->classify(link, Duration::from_seconds(10), at(1));
  if (!first.ok()) {
    std::printf("classification failed: %s\n", first.error().describe().c_str());
    return 1;
  }
  std::printf("phase 1: verdict=%s congestion=%s confidence=%u\n",
              std::string(to_string(first.value().verdict)).c_str(),
              first.value().congestion_asserted ? "yes" : "no", first.value().confidence);
  for (const Blocker& blocker : first.value().blockers) {
    std::printf("  blocker %s: %s\n", blocker.code.c_str(), blocker.detail.c_str());
  }

  // A drop counter is direct evidence of harm: only now is congestion asserted.
  EvidenceRecord drops = make_record(source, generation, ++sequence, EvidenceKind::kDropCount, link,
                                     0.002, ObservationUnit::kRatio, ValueSemantics::kRate, 1);
  if (!runtime.value()->ingest(drops, at(1)).ok()) {
    std::printf("drop ingestion failed\n");
    return 1;
  }

  auto second = runtime.value()->classify(link, Duration::from_seconds(10), at(2));
  if (!second.ok()) {
    std::printf("classification failed: %s\n", second.error().describe().c_str());
    return 1;
  }
  std::printf("phase 2: verdict=%s congestion=%s mechanisms=%s\n",
              std::string(to_string(second.value().verdict)).c_str(),
              second.value().congestion_asserted ? "yes" : "no",
              describe_mechanisms(second.value().mechanisms).c_str());
  std::printf("%s", second.value().explanation.render().c_str());

  EpisodeUpdate update;
  auto evaluated = runtime.value()->evaluate(link, Duration::from_seconds(10), at(2), &update);
  if (!evaluated.ok()) {
    std::printf("evaluation failed: %s\n", evaluated.error().describe().c_str());
    return 1;
  }
  std::printf("episode %s state=%s revision=%llu\n", update.id.str().c_str(),
              std::string(to_string(update.state)).c_str(),
              static_cast<unsigned long long>(update.revision.value()));

  bool ok = !first.value().congestion_asserted && second.value().congestion_asserted;
  std::printf("example minimal_pipeline: %s\n", ok ? "ok" : "unexpected outcome");
  return ok ? 0 : 1;
}
