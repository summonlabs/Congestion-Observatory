// Congestion Observatory - generation-tagged fabric topology model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_MODEL_TOPOLOGY_HPP
#define CONGESTION_MODEL_TOPOLOGY_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/model/generation.hpp"
#include "congestion/model/identities.hpp"

namespace congestion {

enum class NodeKind : std::uint8_t {
  kUnknown = 0,
  kSwitch = 1,
  kHost = 2,
  kNic = 3,
  kRouter = 4,
  kAppliance = 5,
};

[[nodiscard]] std::string_view to_string(NodeKind kind) noexcept;
[[nodiscard]] Result<NodeKind> node_kind_from_string(std::string_view text);

struct Node {
  NodeId id{};
  NodeKind kind{NodeKind::kUnknown};
  std::vector<Name> labels{};
};

struct Port {
  PortId id{};
  NodeId node{};
  std::uint64_t speed_bps{0};
};

struct Link {
  LinkId id{};
  PortId endpoint_a{};
  PortId endpoint_b{};
  std::uint64_t capacity_bps{0};
};

struct Queue {
  QueueId id{};
  LinkId link{};
  std::uint32_t index{0};
  std::uint32_t min_share_bp{0};  // basis points of link capacity, 0..10000
  std::uint32_t max_share_bp{10000};
};

struct Buffer {
  BufferId id{};
  PortId port{};
  std::uint32_t pool{0};
  std::uint64_t cells{0};
};

struct Path {
  PathId id{};
  std::vector<LinkId> hops{};
};

struct Flow {
  FlowId id{};
  PathId path{};
  TenantId tenant{};
  ClassId traffic_class{};
};

class TopologyBuilder;

// An immutable, generation-tagged view of the fabric. Evidence is only ever correlated against
// a topology whose GenerationVector matches the evidence provenance: correlating across
// generations would silently attribute behaviour to objects that may no longer exist.
class Topology {
 public:
  Topology() = default;

  [[nodiscard]] const GenerationVector& generation() const noexcept { return generation_; }
  [[nodiscard]] const Name& revision_label() const noexcept { return revision_label_; }

  [[nodiscard]] const std::map<Name, Node>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] const std::map<Name, Port>& ports() const noexcept { return ports_; }
  [[nodiscard]] const std::map<Name, Link>& links() const noexcept { return links_; }
  [[nodiscard]] const std::map<Name, Queue>& queues() const noexcept { return queues_; }
  [[nodiscard]] const std::map<Name, Buffer>& buffers() const noexcept { return buffers_; }
  [[nodiscard]] const std::map<Name, Path>& paths() const noexcept { return paths_; }
  [[nodiscard]] const std::map<Name, Flow>& flows() const noexcept { return flows_; }

  [[nodiscard]] const Node* node(const NodeId& id) const noexcept;
  [[nodiscard]] const Port* port(const PortId& id) const noexcept;
  [[nodiscard]] const Link* link(const LinkId& id) const noexcept;
  [[nodiscard]] const Queue* queue(const QueueId& id) const noexcept;
  [[nodiscard]] const Buffer* buffer(const BufferId& id) const noexcept;
  [[nodiscard]] const Path* path(const PathId& id) const noexcept;
  [[nodiscard]] const Flow* flow(const FlowId& id) const noexcept;

  [[nodiscard]] const Node* node_of_port(const PortId& id) const noexcept;
  [[nodiscard]] const Link* link_of_queue(const QueueId& id) const noexcept;
  [[nodiscard]] const Port* port_of_buffer(const BufferId& id) const noexcept;

  // Sorted adjacency queries. Every returned vector is in ascending identity order.
  [[nodiscard]] std::vector<LinkId> links_of_node(const NodeId& id) const;
  [[nodiscard]] std::vector<PathId> paths_through_link(const LinkId& id) const;
  [[nodiscard]] std::vector<FlowId> flows_on_path(const PathId& id) const;
  [[nodiscard]] std::vector<QueueId> queues_of_link(const LinkId& id) const;
  [[nodiscard]] std::vector<BufferId> buffers_of_port(const PortId& id) const;

  // True when the subject exists in this topology.
  [[nodiscard]] bool contains(const EvidenceSubject& subject) const noexcept;

  // Explains why a subject is not present. Returns kOk when it is present.
  [[nodiscard]] Status require(const EvidenceSubject& subject) const;

  [[nodiscard]] std::size_t total_objects() const noexcept;

  // Deterministic digest over the canonical encoding of the whole topology.
  [[nodiscard]] std::uint64_t digest() const noexcept;

 private:
  friend class TopologyBuilder;

  GenerationVector generation_{};
  Name revision_label_{};
  std::map<Name, Node> nodes_{};
  std::map<Name, Port> ports_{};
  std::map<Name, Link> links_{};
  std::map<Name, Queue> queues_{};
  std::map<Name, Buffer> buffers_{};
  std::map<Name, Path> paths_{};
  std::map<Name, Flow> flows_{};
  std::map<Name, Name> port_to_node_{};
  std::map<Name, Name> queue_to_link_{};
  std::map<Name, Name> buffer_to_port_{};
  std::map<Name, std::vector<Name>> node_to_links_{};
  std::map<Name, std::vector<Name>> link_to_paths_{};
  std::map<Name, std::vector<Name>> path_to_flows_{};
  std::map<Name, std::vector<Name>> link_to_queues_{};
  std::map<Name, std::vector<Name>> port_to_buffers_{};
};

// Builds a topology with limit enforcement and full referential validation. build() never
// returns a partially valid topology: any error yields no object.
class TopologyBuilder {
 public:
  TopologyBuilder() = default;

  void set_generation(GenerationVector generation) noexcept { generation_ = generation; }
  void set_revision_label(Name label) noexcept { revision_label_ = std::move(label); }
  // Bounds are enforced while the topology is assembled, not only when it is finished.
  void set_limits(const Limits& limits) noexcept { limits_ = limits; }

  [[nodiscard]] Status add_node(const Node& node);
  [[nodiscard]] Status add_port(const Port& port);
  [[nodiscard]] Status add_link(const Link& link);
  [[nodiscard]] Status add_queue(const Queue& queue);
  [[nodiscard]] Status add_buffer(const Buffer& buffer);
  [[nodiscard]] Status add_path(const Path& path);
  [[nodiscard]] Status add_flow(const Flow& flow);

  [[nodiscard]] Result<Topology> build(const Limits& limits) const;

 private:
  GenerationVector generation_{};
  Name revision_label_{};
  Limits limits_{};
  std::map<Name, Node> nodes_{};
  std::map<Name, Port> ports_{};
  std::map<Name, Link> links_{};
  std::map<Name, Queue> queues_{};
  std::map<Name, Buffer> buffers_{};
  std::map<Name, Path> paths_{};
  std::map<Name, Flow> flows_{};
};

}  // namespace congestion

#endif  // CONGESTION_MODEL_TOPOLOGY_HPP
