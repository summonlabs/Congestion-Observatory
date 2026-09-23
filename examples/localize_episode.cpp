// Congestion Observatory - causal localization and episode correlation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Demonstrates the localization contract: structural adjacency is not a cause. A culprit is only
// reported when the path to it is backed by evidence and the candidate has decisive evidence of
// its own.

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
  const char* nodes[] = {"leaf-a", "leaf-b", "leaf-c"};
  for (const char* name : nodes) {
    congestion::Node node;
    node.id = congestion::NodeId::unchecked(name);
    node.kind = congestion::NodeKind::kSwitch;
    (void)builder.add_node(node);
  }
  struct PortSpec { const char* id; const char* node; };
  const PortSpec ports[] = {{"leaf-a/1", "leaf-a"}, {"leaf-b/1", "leaf-b"}, {"leaf-b/2", "leaf-b"},
                            {"leaf-c/1", "leaf-c"}};
  for (const PortSpec& spec : ports) {
    congestion::Port port;
    port.id = congestion::PortId::unchecked(spec.id);
    port.node = congestion::NodeId::unchecked(spec.node);
    port.speed_bps = 100000000000ULL;
    (void)builder.add_port(port);
  }
  struct LinkSpec { const char* id; const char* a; const char* b; };
  const LinkSpec links[] = {{"a-b", "leaf-a/1", "leaf-b/1"}, {"b-c", "leaf-b/2", "leaf-c/1"}};
  for (const LinkSpec& spec : links) {
    congestion::Link link;
    link.id = congestion::LinkId::unchecked(spec.id);
    link.endpoint_a = congestion::PortId::unchecked(spec.a);
    link.endpoint_b = congestion::PortId::unchecked(spec.b);
    link.capacity_bps = 100000000000ULL;
    (void)builder.add_link(link);
  }
  // The path makes a-b upstream of b-c, which is what lets the localizer walk from the congested
  // downstream link towards the link that explains it.
  congestion::Path path;
  path.id = congestion::PathId::unchecked("a-c");
  path.hops = {congestion::LinkId::unchecked("a-b"), congestion::LinkId::unchecked("b-c")};
  (void)builder.add_path(path);

  auto topology = builder.build(congestion::Limits{});
  return topology.ok() ? topology.value() : congestion::Topology{};
}

congestion::EvidenceRecord evidence(const congestion::SourceId& source,
                                    const congestion::GenerationVector& generation,
                                    std::uint64_t sequence, congestion::EvidenceKind kind,
                                    const congestion::EvidenceSubject& subject, double value,
                                    congestion::ObservationUnit unit,
                                    congestion::ValueSemantics semantics, std::int64_t offset,
                                    std::uint64_t incarnation = 1) {
  congestion::EvidenceRecord record;
  record.kind = kind;
  record.subject = subject;
  record.provenance.source = source;
  record.provenance.authority = congestion::AuthorityLevel::kMeasured;
  record.fence.source = source;
  record.fence.boot = congestion::BootId(1);
  record.fence.incarnation = congestion::Incarnation(incarnation);
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
  RuntimeConfig config;
  config.topology = topology;
  auto runtime = Runtime::create(std::move(config));
  if (!runtime.ok()) {
    std::printf("runtime creation failed: %s\n", runtime.error().describe().c_str());
    return 1;
  }

  const SourceId source = SourceId::unchecked("example-collector");
  const GenerationVector generation = topology.generation();
  const EvidenceSubject symptom = EvidenceSubject(LinkId::unchecked("b-c"));
  const EvidenceSubject upstream = EvidenceSubject(LinkId::unchecked("a-b"));
  std::uint64_t sequence = 0;

  std::vector<EvidenceRecord> records;
  // Topology advertisements: without them the runtime refuses to invent structural edges.
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kTopologyAdvertisement,
                             symptom, 1.0, ObservationUnit::kBoolean, ValueSemantics::kGauge, 0));
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kTopologyAdvertisement,
                             upstream, 1.0, ObservationUnit::kBoolean, ValueSemantics::kGauge, 0));
  // The symptom itself is suffering.
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kDropCount, symptom,
                             0.01, ObservationUnit::kRatio, ValueSemantics::kRate, 1));
  // The upstream link carries the explanation: demand above capacity and a deep backlog.
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kOfferedDemand, upstream,
                             130000000000.0, ObservationUnit::kBitsPerSecond,
                             ValueSemantics::kGauge, 1));
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kCapacityAdvertisement,
                             upstream, 100000000000.0, ObservationUnit::kBitsPerSecond,
                             ValueSemantics::kGauge, 1));
  records.push_back(evidence(source, generation, ++sequence, EvidenceKind::kQueueOccupancy, upstream,
                             0.92, ObservationUnit::kRatio, ValueSemantics::kGauge, 1));

  for (const EvidenceRecord& record : records) {
    auto outcome = runtime.value()->ingest(record, at(1));
    if (!outcome.ok()) {
      std::printf("ingest failed: %s\n", outcome.error().describe().c_str());
      return 1;
    }
  }
  if (!runtime.value()->rebuild_causal_graph().ok()) {
    std::printf("graph rebuild failed\n");
    return 1;
  }
  std::printf("causal graph: %zu nodes, %zu edges\n", runtime.value()->causal_graph().node_count(),
              runtime.value()->causal_graph().edge_count());

  auto localization = runtime.value()->localize(symptom, Duration::from_seconds(30), at(2));
  if (!localization.ok()) {
    std::printf("localization failed: %s\n", localization.error().describe().c_str());
    return 1;
  }
  std::printf("outcome=%s candidates=%zu\n",
              std::string(to_string(localization.value().outcome)).c_str(),
              localization.value().candidates.size());
  for (const LocalizationCandidate& candidate : localization.value().candidates) {
    std::printf("  %s score=%u confidence=%u depth=%zu\n", candidate.subject.str().c_str(),
                candidate.score, candidate.confidence, candidate.depth);
    for (const std::string& basis : candidate.basis) {
      std::printf("    %s\n", basis.c_str());
    }
  }

  EpisodeUpdate update;
  auto evaluated = runtime.value()->evaluate(symptom, Duration::from_seconds(30), at(2), &update);
  if (!evaluated.ok()) {
    std::printf("evaluation failed: %s\n", evaluated.error().describe().c_str());
    return 1;
  }
  auto groups = runtime.value()->correlate(at(2));
  if (!groups.ok()) {
    std::printf("correlation failed: %s\n", groups.error().describe().c_str());
    return 1;
  }
  std::printf("episode %s, correlation groups=%zu\n", update.id.str().c_str(),
              groups.value().size());

  const bool localized = localization.value().outcome == LocalizationOutcome::kLocalized &&
                         !localization.value().candidates.empty() &&
                         localization.value().candidates.front().subject == upstream;
  std::printf("example localize_episode: %s\n", localized ? "ok" : "unexpected outcome");
  return localized ? 0 : 1;
}
