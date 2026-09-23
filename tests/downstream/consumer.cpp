// Congestion Observatory - independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is deliberately written the way an external project would be: it includes the
// installed public headers, links CongestionObservatory::congestion_observatory and exercises the
// documented surface. The package proof builds it twice: once inside this repository and once as
// a standalone project that resolves the package with find_package(CongestionObservatory CONFIG).

#include <cstdio>
#include <string>
#include <vector>

#include "congestion/congestion.hpp"

int main() {
  using namespace congestion;

  Limits limits;
  if (!limits.validate().ok()) {
    std::printf("downstream: default limits are invalid\n");
    return 1;
  }

  auto link = LinkId::parse("leaf1-leaf2");
  if (!link.ok()) {
    std::printf("downstream: link identity rejected\n");
    return 1;
  }

  SourceId source = SourceId::unchecked("downstream-collector");
  EvidenceRecord record;
  record.kind = EvidenceKind::kDropCount;
  record.subject = EvidenceSubject(link.value());
  record.provenance.source = source;
  record.provenance.authority = AuthorityLevel::kMeasured;
  record.fence.source = source;
  record.fence.boot = BootId(1);
  record.fence.incarnation = Incarnation(1);
  record.fence.gen.epoch = Epoch(1);
  record.fence.gen.generation = Generation(1);
  record.fence.gen.revision = Revision(1);
  record.fence.sequence = Sequence(1);
  record.observed_at = Timestamp::from_unix_seconds(1000).value();
  record.received_at = record.observed_at;
  record.clock = ClockDomain::kCollectorWallClock;
  record.value.scalar = 0.5;
  record.value.unit = ObservationUnit::kRatio;
  record.value.semantics = ValueSemantics::kGauge;
  record.id = compute_evidence_id(record);

  ClassificationRequest request;
  request.subject = record.subject;
  request.window_start = Timestamp::from_unix_seconds(990).value();
  request.window_end = Timestamp::from_unix_seconds(1001).value();
  request.evaluated_at = Timestamp::from_unix_seconds(1001).value();
  request.generation = record.fence.gen;

  const std::vector<const EvidenceRecord*> pointers{&record};
  auto assessment = classify_subject(request, pointers, ClassificationPolicy{});
  if (!assessment.ok()) {
    std::printf("downstream: classification failed: %s\n", assessment.error().describe().c_str());
    return 1;
  }
  if (!assessment.value().congestion_asserted) {
    std::printf("downstream: fresh drop evidence did not assert congestion\n");
    return 1;
  }

  // Utilization alone must never be congestion.
  EvidenceRecord utilization = record;
  utilization.kind = EvidenceKind::kLinkUtilization;
  utilization.value.scalar = 0.99;
  utilization.id = compute_evidence_id(utilization);
  const std::vector<const EvidenceRecord*> utilization_only{&utilization};
  auto healthy = classify_subject(request, utilization_only, ClassificationPolicy{});
  if (!healthy.ok() || healthy.value().congestion_asserted) {
    std::printf("downstream: utilization alone was treated as congestion\n");
    return 1;
  }

  // The causal graph refuses an edge without evidence.
  CausalGraph graph;
  CausalEdge edge;
  edge.from = EvidenceSubject(LinkId::unchecked("a"));
  edge.to = EvidenceSubject(LinkId::unchecked("b"));
  edge.rule_id = "downstream-rule";
  if (graph.add_edge(edge, limits).ok()) {
    std::printf("downstream: an evidence free causal edge was accepted\n");
    return 1;
  }
  edge.citations.push_back(record.id);
  if (!graph.add_edge(edge, limits).ok()) {
    std::printf("downstream: an evidence backed causal edge was rejected\n");
    return 1;
  }

  std::printf("downstream: %s verdict=%s product=%s\n", version_string().c_str(),
              std::string(to_string(healthy.value().verdict)).c_str(),
              std::string(product_id()).c_str());
  return 0;
}
