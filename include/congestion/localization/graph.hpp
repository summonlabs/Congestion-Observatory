// Congestion Observatory - evidence-backed causal graph.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_LOCALIZATION_GRAPH_HPP
#define CONGESTION_LOCALIZATION_GRAPH_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/model/generation.hpp"
#include "congestion/model/identities.hpp"

namespace congestion {

// The kind of relation an edge encodes. Structural kinds come from the topology; observational
// kinds require measurement on both endpoints in the same window.
enum class CausalEdgeKind : std::uint8_t {
  kTopologyAdjacency = 0,   // two objects are physically adjacent
  kContainment = 1,         // one object is part of another (queue of a link, port of a node)
  kPathMembership = 2,      // an object is traversed by a path
  kQueueOnLink = 3,
  kBufferOnPort = 4,
  kFlowOnPath = 5,
  kDemandUpstream = 6,      // measured demand rise at the source precedes impairment at the sink
  kSharedContention = 7,    // both objects are contended by the same measured consumer set
  kLatencyPropagation = 8,  // measured latency inflation at the source explains the sink
  kBufferPressure = 9,      // measured buffer occupancy at the source explains queue pressure
};

[[nodiscard]] std::string_view to_string(CausalEdgeKind kind) noexcept;
[[nodiscard]] bool is_observational(CausalEdgeKind kind) noexcept;

// An edge is only ever created together with the evidence that justifies it. A non-empty citation
// list is a hard precondition: add_edge() rejects an empty list with kPreconditionFailed.
struct CausalEdge {
  EvidenceSubject from{};
  EvidenceSubject to{};
  CausalEdgeKind kind{CausalEdgeKind::kTopologyAdjacency};
  std::string rule_id{};
  std::vector<EvidenceId> citations{};
  std::uint32_t strength{1};  // 1..100 ordinal, deterministic, deliberately not a probability
  bool observational{false};
  GenerationVector generation{};

  friend bool operator==(const CausalEdge& lhs, const CausalEdge& rhs) noexcept;
};

struct CausalEdgeKey {
  Name from{};
  Name to{};
  CausalEdgeKind kind{CausalEdgeKind::kTopologyAdjacency};
  Name rule{};

  friend bool operator<(const CausalEdgeKey& lhs, const CausalEdgeKey& rhs) noexcept {
    if (lhs.from != rhs.from) return lhs.from < rhs.from;
    if (lhs.to != rhs.to) return lhs.to < rhs.to;
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.rule < rhs.rule;
  }
};

// Immutable-once-built graph. All mutation goes through add_edge(), which enforces the evidence
// precondition and the configured bounds. Iteration order is always deterministic.
class CausalGraph {
 public:
  CausalGraph() = default;

  [[nodiscard]] Status add_node(const EvidenceSubject& subject, const Limits& limits);
  [[nodiscard]] Status add_edge(const CausalEdge& edge, const Limits& limits);

  [[nodiscard]] bool has_node(const EvidenceSubject& subject) const noexcept;
  [[nodiscard]] const CausalEdge* find_edge(const EvidenceSubject& from,
                                            const EvidenceSubject& to) const noexcept;
  // All outgoing edges of a subject, ordered by (to, kind, rule).
  [[nodiscard]] std::vector<CausalEdge> out_edges(const EvidenceSubject& from) const;
  [[nodiscard]] std::vector<CausalEdge> in_edges(const EvidenceSubject& to) const;

  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
  [[nodiscard]] const std::map<Name, EvidenceSubject>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] std::uint64_t digest() const noexcept;

 private:
  std::map<Name, EvidenceSubject> nodes_{};
  std::map<CausalEdgeKey, CausalEdge> edges_{};
  std::map<Name, std::vector<Name>> out_adjacency_{};
  std::map<Name, std::vector<Name>> in_adjacency_{};
};

}  // namespace congestion

#endif  // CONGESTION_LOCALIZATION_GRAPH_HPP
