// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/model/topology.hpp"

#include <algorithm>

namespace congestion {
namespace {

Status capacity_check(std::size_t size, std::size_t maximum, const char* what) {
  if (size >= maximum) {
    return Status(make_error(ErrorCode::kLimitExceeded,
                             std::string("topology ") + what + " limit reached",
                             "limit=" + std::to_string(maximum)));
  }
  return Status{};
}

template <class Map>
Status duplicate_check(const Map& map, const Name& id, const char* what) {
  if (id.valid() && map.find(id) != map.end()) {
    return Status(make_error(ErrorCode::kAlreadyExists,
                             std::string("duplicate ") + what + " identity", id.str()));
  }
  if (!id.valid()) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             std::string("invalid ") + what + " identity"));
  }
  return Status{};
}

}  // namespace

std::string_view to_string(NodeKind kind) noexcept {
  switch (kind) {
    case NodeKind::kUnknown: return "unknown";
    case NodeKind::kSwitch: return "switch";
    case NodeKind::kHost: return "host";
    case NodeKind::kNic: return "nic";
    case NodeKind::kRouter: return "router";
    case NodeKind::kAppliance: return "appliance";
  }
  return "unknown";
}

Result<NodeKind> node_kind_from_string(std::string_view text) {
  if (text == "unknown") return NodeKind::kUnknown;
  if (text == "switch") return NodeKind::kSwitch;
  if (text == "host") return NodeKind::kHost;
  if (text == "nic") return NodeKind::kNic;
  if (text == "router") return NodeKind::kRouter;
  if (text == "appliance") return NodeKind::kAppliance;
  return make_error(ErrorCode::kInvalidArgument, "unknown node kind", std::string(text));
}

const Node* Topology::node(const NodeId& id) const noexcept {
  const auto it = nodes_.find(id.name());
  return it == nodes_.end() ? nullptr : &it->second;
}

const Port* Topology::port(const PortId& id) const noexcept {
  const auto it = ports_.find(id.name());
  return it == ports_.end() ? nullptr : &it->second;
}

const Link* Topology::link(const LinkId& id) const noexcept {
  const auto it = links_.find(id.name());
  return it == links_.end() ? nullptr : &it->second;
}

const Queue* Topology::queue(const QueueId& id) const noexcept {
  const auto it = queues_.find(id.name());
  return it == queues_.end() ? nullptr : &it->second;
}

const Buffer* Topology::buffer(const BufferId& id) const noexcept {
  const auto it = buffers_.find(id.name());
  return it == buffers_.end() ? nullptr : &it->second;
}

const Path* Topology::path(const PathId& id) const noexcept {
  const auto it = paths_.find(id.name());
  return it == paths_.end() ? nullptr : &it->second;
}

const Flow* Topology::flow(const FlowId& id) const noexcept {
  const auto it = flows_.find(id.name());
  return it == flows_.end() ? nullptr : &it->second;
}

const Node* Topology::node_of_port(const PortId& id) const noexcept {
  const auto it = port_to_node_.find(id.name());
  if (it == port_to_node_.end()) {
    return nullptr;
  }
  return node(NodeId(it->second));
}

const Link* Topology::link_of_queue(const QueueId& id) const noexcept {
  const auto it = queue_to_link_.find(id.name());
  if (it == queue_to_link_.end()) {
    return nullptr;
  }
  return link(LinkId(it->second));
}

const Port* Topology::port_of_buffer(const BufferId& id) const noexcept {
  const auto it = buffer_to_port_.find(id.name());
  if (it == buffer_to_port_.end()) {
    return nullptr;
  }
  return port(PortId(it->second));
}

std::vector<LinkId> Topology::links_of_node(const NodeId& id) const {
  std::vector<LinkId> out;
  const auto it = node_to_links_.find(id.name());
  if (it == node_to_links_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const Name& name : it->second) {
    out.emplace_back(name);
  }
  return out;
}

std::vector<PathId> Topology::paths_through_link(const LinkId& id) const {
  std::vector<PathId> out;
  const auto it = link_to_paths_.find(id.name());
  if (it == link_to_paths_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const Name& name : it->second) {
    out.emplace_back(name);
  }
  return out;
}

std::vector<FlowId> Topology::flows_on_path(const PathId& id) const {
  std::vector<FlowId> out;
  const auto it = path_to_flows_.find(id.name());
  if (it == path_to_flows_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const Name& name : it->second) {
    out.emplace_back(name);
  }
  return out;
}

std::vector<QueueId> Topology::queues_of_link(const LinkId& id) const {
  std::vector<QueueId> out;
  const auto it = link_to_queues_.find(id.name());
  if (it == link_to_queues_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const Name& name : it->second) {
    out.emplace_back(name);
  }
  return out;
}

std::vector<BufferId> Topology::buffers_of_port(const PortId& id) const {
  std::vector<BufferId> out;
  const auto it = port_to_buffers_.find(id.name());
  if (it == port_to_buffers_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const Name& name : it->second) {
    out.emplace_back(name);
  }
  return out;
}

bool Topology::contains(const EvidenceSubject& subject) const noexcept {
  switch (subject.kind()) {
    case SubjectKind::kNode: return node(*subject.get_if<NodeId>()) != nullptr;
    case SubjectKind::kPort: return port(*subject.get_if<PortId>()) != nullptr;
    case SubjectKind::kLink: return link(*subject.get_if<LinkId>()) != nullptr;
    case SubjectKind::kQueue: return queue(*subject.get_if<QueueId>()) != nullptr;
    case SubjectKind::kBuffer: return buffer(*subject.get_if<BufferId>()) != nullptr;
    case SubjectKind::kPath: return path(*subject.get_if<PathId>()) != nullptr;
    case SubjectKind::kFlow: return flow(*subject.get_if<FlowId>()) != nullptr;
    case SubjectKind::kTenant:
    case SubjectKind::kClass:
    case SubjectKind::kSource:
      // Tenants, classes and sources are evidence level objects: they are not topology entities.
      return true;
    case SubjectKind::kTopology: return subject.get_if<TopologyId>() != nullptr;
    case SubjectKind::kNone:
    default: return false;
  }
}

Status Topology::require(const EvidenceSubject& subject) const {
  if (subject.kind() == SubjectKind::kNone) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evidence subject is empty"));
  }
  if (contains(subject)) {
    return Status{};
  }
  return Status(make_error(ErrorCode::kNotFound, "subject is not present in this topology revision",
                           subject.str()));
}

std::size_t Topology::total_objects() const noexcept {
  return nodes_.size() + ports_.size() + links_.size() + queues_.size() + buffers_.size() +
         paths_.size() + flows_.size();
}

std::uint64_t Topology::digest() const noexcept {
  StableHasher hasher;
  hasher.update_u64(generation_.epoch.value());
  hasher.update_u64(generation_.generation.value());
  hasher.update_u64(generation_.revision.value());
  hasher.update(revision_label_.view());
  hasher.separator();
  for (const auto& entry : nodes_) {
    hasher.update(entry.second.id.view());
    hasher.update_u64(static_cast<std::uint64_t>(entry.second.kind));
    for (const Name& label : entry.second.labels) {
      hasher.update(label.view());
    }
    hasher.separator();
  }
  for (const auto& entry : ports_) {
    hasher.update(entry.second.id.view());
    hasher.update(entry.second.node.view());
    hasher.update_u64(entry.second.speed_bps);
  }
  for (const auto& entry : links_) {
    hasher.update(entry.second.id.view());
    hasher.update(entry.second.endpoint_a.view());
    hasher.update(entry.second.endpoint_b.view());
    hasher.update_u64(entry.second.capacity_bps);
  }
  for (const auto& entry : queues_) {
    hasher.update(entry.second.id.view());
    hasher.update(entry.second.link.view());
    hasher.update_u64(entry.second.index);
    hasher.update_u64(entry.second.min_share_bp);
    hasher.update_u64(entry.second.max_share_bp);
  }
  for (const auto& entry : buffers_) {
    hasher.update(entry.second.id.view());
    hasher.update(entry.second.port.view());
    hasher.update_u64(entry.second.pool);
    hasher.update_u64(entry.second.cells);
  }
  for (const auto& entry : paths_) {
    hasher.update(entry.second.id.view());
    for (const LinkId& hop : entry.second.hops) {
      hasher.update(hop.view());
    }
    hasher.separator();
  }
  for (const auto& entry : flows_) {
    hasher.update(entry.second.id.view());
    hasher.update(entry.second.path.view());
    hasher.update(entry.second.tenant.view());
    hasher.update(entry.second.traffic_class.view());
  }
  return hasher.digest64();
}

Status TopologyBuilder::add_node(const Node& node) {
  const Status duplicate = duplicate_check(nodes_, node.id.name(), "node");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(nodes_.size(), limits_.max_nodes, "node");
  if (!capacity.ok()) {
    return capacity;
  }
  if (node.labels.size() > limits_.max_labels) {
    return Status(make_error(ErrorCode::kLimitExceeded, "node label count exceeds the limit"));
  }
  nodes_.emplace(node.id.name(), node);
  return Status{};
}

Status TopologyBuilder::add_port(const Port& port) {
  const Status duplicate = duplicate_check(ports_, port.id.name(), "port");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(ports_.size(), limits_.max_ports, "port");
  if (!capacity.ok()) {
    return capacity;
  }
  if (nodes_.find(port.node.name()) == nodes_.end()) {
    return Status(make_error(ErrorCode::kNotFound, "port references an unknown node",
                             port.node.str()));
  }
  std::size_t ports_on_node = 0;
  for (const auto& entry : ports_) {
    if (entry.second.node == port.node) {
      ++ports_on_node;
    }
  }
  if (ports_on_node >= limits_.max_ports_per_node) {
    return Status(make_error(ErrorCode::kLimitExceeded, "node port limit reached", port.node.str()));
  }
  ports_.emplace(port.id.name(), port);
  return Status{};
}

Status TopologyBuilder::add_link(const Link& link) {
  const Status duplicate = duplicate_check(links_, link.id.name(), "link");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(links_.size(), limits_.max_links, "link");
  if (!capacity.ok()) {
    return capacity;
  }
  if (link.endpoint_a == link.endpoint_b) {
    return Status(make_error(ErrorCode::kInvalidArgument, "link endpoints must differ",
                             link.id.str()));
  }
  if (ports_.find(link.endpoint_a.name()) == ports_.end() ||
      ports_.find(link.endpoint_b.name()) == ports_.end()) {
    return Status(make_error(ErrorCode::kNotFound, "link references an unknown port",
                             link.id.str()));
  }
  links_.emplace(link.id.name(), link);
  return Status{};
}

Status TopologyBuilder::add_queue(const Queue& queue) {
  const Status duplicate = duplicate_check(queues_, queue.id.name(), "queue");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(queues_.size(), limits_.max_queues, "queue");
  if (!capacity.ok()) {
    return capacity;
  }
  if (links_.find(queue.link.name()) == links_.end()) {
    return Status(make_error(ErrorCode::kNotFound, "queue references an unknown link",
                             queue.link.str()));
  }
  if (queue.min_share_bp > queue.max_share_bp || queue.max_share_bp > 10000) {
    return Status(make_error(ErrorCode::kOutOfRange, "queue share bounds are inconsistent",
                             queue.id.str()));
  }
  queues_.emplace(queue.id.name(), queue);
  return Status{};
}

Status TopologyBuilder::add_buffer(const Buffer& buffer) {
  const Status duplicate = duplicate_check(buffers_, buffer.id.name(), "buffer");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(buffers_.size(), limits_.max_buffers, "buffer");
  if (!capacity.ok()) {
    return capacity;
  }
  if (ports_.find(buffer.port.name()) == ports_.end()) {
    return Status(make_error(ErrorCode::kNotFound, "buffer references an unknown port",
                             buffer.port.str()));
  }
  buffers_.emplace(buffer.id.name(), buffer);
  return Status{};
}

Status TopologyBuilder::add_path(const Path& path) {
  const Status duplicate = duplicate_check(paths_, path.id.name(), "path");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(paths_.size(), limits_.max_paths, "path");
  if (!capacity.ok()) {
    return capacity;
  }
  if (path.hops.empty()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "path must contain at least one hop",
                             path.id.str()));
  }
  if (path.hops.size() > limits_.max_path_hops) {
    return Status(make_error(ErrorCode::kLimitExceeded, "path hop count exceeds the limit",
                             path.id.str()));
  }
  for (const LinkId& hop : path.hops) {
    if (links_.find(hop.name()) == links_.end()) {
      return Status(make_error(ErrorCode::kNotFound, "path references an unknown link", hop.str()));
    }
  }
  paths_.emplace(path.id.name(), path);
  return Status{};
}

Status TopologyBuilder::add_flow(const Flow& flow) {
  const Status duplicate = duplicate_check(flows_, flow.id.name(), "flow");
  if (!duplicate.ok()) {
    return duplicate;
  }
  const Status capacity = capacity_check(flows_.size(), limits_.max_flows, "flow");
  if (!capacity.ok()) {
    return capacity;
  }
  if (paths_.find(flow.path.name()) == paths_.end()) {
    return Status(make_error(ErrorCode::kNotFound, "flow references an unknown path",
                             flow.path.str()));
  }
  flows_.emplace(flow.id.name(), flow);
  return Status{};
}

Result<Topology> TopologyBuilder::build(const Limits& limits) const {
  Topology topology;
  topology.generation_ = generation_;
  topology.revision_label_ = revision_label_;

  if (nodes_.size() > limits.max_nodes || ports_.size() > limits.max_ports ||
      links_.size() > limits.max_links || queues_.size() > limits.max_queues ||
      buffers_.size() > limits.max_buffers || paths_.size() > limits.max_paths ||
      flows_.size() > limits.max_flows) {
    return make_error(ErrorCode::kLimitExceeded, "topology exceeds the configured bounds");
  }

  // Validate references once more: a builder may have been populated directly by a caller that
  // bypassed the add_* helpers during deserialisation.
  for (const auto& entry : ports_) {
    if (nodes_.find(entry.second.node.name()) == nodes_.end()) {
      return make_error(ErrorCode::kNotFound, "port references an unknown node",
                        entry.second.id.str());
    }
  }
  for (const auto& entry : links_) {
    if (ports_.find(entry.second.endpoint_a.name()) == ports_.end() ||
        ports_.find(entry.second.endpoint_b.name()) == ports_.end()) {
      return make_error(ErrorCode::kNotFound, "link references an unknown port",
                        entry.second.id.str());
    }
  }
  for (const auto& entry : queues_) {
    if (links_.find(entry.second.link.name()) == links_.end()) {
      return make_error(ErrorCode::kNotFound, "queue references an unknown link",
                        entry.second.id.str());
    }
  }
  for (const auto& entry : buffers_) {
    if (ports_.find(entry.second.port.name()) == ports_.end()) {
      return make_error(ErrorCode::kNotFound, "buffer references an unknown port",
                        entry.second.id.str());
    }
  }
  for (const auto& entry : paths_) {
    for (const LinkId& hop : entry.second.hops) {
      if (links_.find(hop.name()) == links_.end()) {
        return make_error(ErrorCode::kNotFound, "path references an unknown link", hop.str());
      }
    }
  }
  for (const auto& entry : flows_) {
    if (paths_.find(entry.second.path.name()) == paths_.end()) {
      return make_error(ErrorCode::kNotFound, "flow references an unknown path",
                        entry.second.id.str());
    }
  }

  topology.nodes_ = nodes_;
  topology.ports_ = ports_;
  topology.links_ = links_;
  topology.queues_ = queues_;
  topology.buffers_ = buffers_;
  topology.paths_ = paths_;
  topology.flows_ = flows_;

  for (const auto& entry : ports_) {
    topology.port_to_node_.emplace(entry.first, entry.second.node.name());
  }
  for (const auto& entry : queues_) {
    topology.queue_to_link_.emplace(entry.first, entry.second.link.name());
    topology.link_to_queues_[entry.second.link.name()].push_back(entry.first);
  }
  for (const auto& entry : buffers_) {
    topology.buffer_to_port_.emplace(entry.first, entry.second.port.name());
    topology.port_to_buffers_[entry.second.port.name()].push_back(entry.first);
  }
  for (const auto& entry : links_) {
    const auto endpoint_a = topology.port_to_node_.find(entry.second.endpoint_a.name());
    const auto endpoint_b = topology.port_to_node_.find(entry.second.endpoint_b.name());
    if (endpoint_a != topology.port_to_node_.end()) {
      topology.node_to_links_[endpoint_a->second].push_back(entry.first);
    }
    if (endpoint_b != topology.port_to_node_.end()) {
      topology.node_to_links_[endpoint_b->second].push_back(entry.first);
    }
  }
  for (auto& entry : topology.node_to_links_) {
    auto& links = entry.second;
    links.erase(std::unique(links.begin(), links.end()), links.end());
    std::sort(links.begin(), links.end());
  }
  for (const auto& entry : paths_) {
    for (const LinkId& hop : entry.second.hops) {
      topology.link_to_paths_[hop.name()].push_back(entry.first);
    }
  }
  for (auto& entry : topology.link_to_paths_) {
    auto& paths = entry.second;
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::sort(paths.begin(), paths.end());
  }
  for (const auto& entry : flows_) {
    topology.path_to_flows_[entry.second.path.name()].push_back(entry.first);
  }
  for (auto& entry : topology.path_to_flows_) {
    auto& flows = entry.second;
    flows.erase(std::unique(flows.begin(), flows.end()), flows.end());
    std::sort(flows.begin(), flows.end());
  }
  for (auto& entry : topology.link_to_queues_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : topology.port_to_buffers_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  return topology;
}

}  // namespace congestion
