// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/localization/graph.hpp"

#include <algorithm>

namespace congestion {

bool operator==(const CausalEdge& lhs, const CausalEdge& rhs) noexcept {
  return lhs.from == rhs.from && lhs.to == rhs.to && lhs.kind == rhs.kind &&
         lhs.rule_id == rhs.rule_id && lhs.citations == rhs.citations &&
         lhs.strength == rhs.strength && lhs.observational == rhs.observational &&
         lhs.generation == rhs.generation;
}

std::string_view to_string(CausalEdgeKind kind) noexcept {
  switch (kind) {
    case CausalEdgeKind::kTopologyAdjacency: return "topology_adjacency";
    case CausalEdgeKind::kContainment: return "containment";
    case CausalEdgeKind::kPathMembership: return "path_membership";
    case CausalEdgeKind::kQueueOnLink: return "queue_on_link";
    case CausalEdgeKind::kBufferOnPort: return "buffer_on_port";
    case CausalEdgeKind::kFlowOnPath: return "flow_on_path";
    case CausalEdgeKind::kDemandUpstream: return "demand_upstream";
    case CausalEdgeKind::kSharedContention: return "shared_contention";
    case CausalEdgeKind::kLatencyPropagation: return "latency_propagation";
    case CausalEdgeKind::kBufferPressure: return "buffer_pressure";
  }
  return "topology_adjacency";
}

bool is_observational(CausalEdgeKind kind) noexcept {
  switch (kind) {
    case CausalEdgeKind::kDemandUpstream:
    case CausalEdgeKind::kSharedContention:
    case CausalEdgeKind::kLatencyPropagation:
    case CausalEdgeKind::kBufferPressure:
      return true;
    default:
      return false;
  }
}

Status CausalGraph::add_node(const EvidenceSubject& subject, const Limits& limits) {
  if (!subject.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "causal node subject is empty"));
  }
  const Name key = Name::unchecked(subject.str());
  if (!key.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "causal node subject is not canonical",
                             subject.str()));
  }
  if (nodes_.find(key) != nodes_.end()) {
    return Status{};
  }
  if (nodes_.size() >= limits.max_graph_nodes) {
    return Status(make_error(ErrorCode::kLimitExceeded, "causal graph node limit reached",
                             std::to_string(limits.max_graph_nodes)));
  }
  nodes_.emplace(key, subject);
  return Status{};
}

Status CausalGraph::add_edge(const CausalEdge& edge, const Limits& limits) {
  // The evidence precondition is enforced here and nowhere else: an edge without citations is
  // not a causal claim.
  if (edge.citations.empty()) {
    return Status(make_error(ErrorCode::kPreconditionFailed,
                             "causal edge requires at least one citation", edge.rule_id));
  }
  if (!edge.from.valid() || !edge.to.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "causal edge endpoints must be set"));
  }
  if (edge.rule_id.empty()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "causal edge requires a rule id"));
  }

  const Name from_key = Name::unchecked(edge.from.str());
  const Name to_key = Name::unchecked(edge.to.str());
  const Name rule_key = Name::unchecked(edge.rule_id);
  if (!from_key.valid() || !to_key.valid() || !rule_key.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "causal edge identifiers must be canonical names", edge.rule_id));
  }

  for (const EvidenceSubject& subject : {edge.from, edge.to}) {
    const Status added = add_node(subject, limits);
    if (!added.ok()) {
      return added;
    }
  }

  CausalEdgeKey key{from_key, to_key, edge.kind, rule_key};
  if (edges_.find(key) != edges_.end()) {
    return Status{};
  }
  if (edges_.size() >= limits.max_graph_edges) {
    return Status(make_error(ErrorCode::kLimitExceeded, "causal graph edge limit reached",
                             std::to_string(limits.max_graph_edges)));
  }
  CausalEdge stored = edge;
  stored.observational = is_observational(edge.kind);
  std::sort(stored.citations.begin(), stored.citations.end());
  stored.citations.erase(std::unique(stored.citations.begin(), stored.citations.end()),
                         stored.citations.end());
  if (stored.citations.size() > limits.max_citations) {
    stored.citations.resize(limits.max_citations);
  }
  edges_.emplace(key, std::move(stored));

  auto& outgoing = out_adjacency_[from_key];
  if (std::find(outgoing.begin(), outgoing.end(), to_key) == outgoing.end()) {
    outgoing.push_back(to_key);
    std::sort(outgoing.begin(), outgoing.end());
  }
  auto& incoming = in_adjacency_[to_key];
  if (std::find(incoming.begin(), incoming.end(), from_key) == incoming.end()) {
    incoming.push_back(from_key);
    std::sort(incoming.begin(), incoming.end());
  }
  return Status{};
}

bool CausalGraph::has_node(const EvidenceSubject& subject) const noexcept {
  return nodes_.find(Name::unchecked(subject.str())) != nodes_.end();
}

const CausalEdge* CausalGraph::find_edge(const EvidenceSubject& from,
                                         const EvidenceSubject& to) const noexcept {
  const Name from_key = Name::unchecked(from.str());
  const Name to_key = Name::unchecked(to.str());
  for (const auto& entry : edges_) {
    if (entry.first.from == from_key && entry.first.to == to_key) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::vector<CausalEdge> CausalGraph::out_edges(const EvidenceSubject& from) const {
  std::vector<CausalEdge> out;
  const Name from_key = Name::unchecked(from.str());
  const CausalEdgeKey lower{from_key, Name(), static_cast<CausalEdgeKind>(0), Name()};
  for (auto it = edges_.lower_bound(lower); it != edges_.end() && it->first.from == from_key; ++it) {
    out.push_back(it->second);
  }
  return out;
}

std::vector<CausalEdge> CausalGraph::in_edges(const EvidenceSubject& to) const {
  std::vector<CausalEdge> out;
  const Name to_key = Name::unchecked(to.str());
  const auto adjacency = in_adjacency_.find(to_key);
  if (adjacency == in_adjacency_.end()) {
    return out;
  }
  for (const Name& from : adjacency->second) {
    const CausalEdgeKey lower{from, Name(), static_cast<CausalEdgeKind>(0), Name()};
    for (auto it = edges_.lower_bound(lower); it != edges_.end() && it->first.from == from; ++it) {
      if (it->first.to == to_key) {
        out.push_back(it->second);
      }
    }
  }
  return out;
}

std::uint64_t CausalGraph::digest() const noexcept {
  StableHasher hasher;
  for (const auto& entry : nodes_) {
    hasher.update(entry.first.view());
    hasher.separator();
  }
  for (const auto& entry : edges_) {
    hasher.update(entry.first.from.view());
    hasher.update(entry.first.to.view());
    hasher.update_u64(static_cast<std::uint64_t>(entry.first.kind));
    hasher.update(entry.first.rule.view());
    hasher.update_u64(entry.second.strength);
    for (const EvidenceId& citation : entry.second.citations) {
      hasher.update(citation.str());
    }
    hasher.separator();
  }
  return hasher.digest64();
}

}  // namespace congestion
