// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/persistence/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "congestion/core/checked.hpp"
#include "congestion/core/crc64.hpp"
#include "congestion/core/hash.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t value) { out_.push_back(value); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }

  void u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }

  void u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void boolean(bool value) { u8(value ? 1u : 0u); }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (const char c : value) {
      u8(static_cast<std::uint8_t>(c));
    }
  }

  void blob(const std::vector<std::uint8_t>& value) {
    u64(static_cast<std::uint64_t>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
  }

  void digest_id(const std::array<std::uint64_t, 2>& value) {
    u64(value[0]);
    u64(value[1]);
  }

 private:
  std::vector<std::uint8_t>& out_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size, const Limits& limits)
      : data_(data), size_(size), limits_(limits) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }

  bool fail(const char* what) {
    if (ok_) {
      ok_ = false;
      failure_ = what;
    }
    return false;
  }

  std::uint8_t u8() {
    if (position_ + 1 > size_) {
      fail("truncated snapshot: byte");
      return 0;
    }
    return data_[position_++];
  }

  std::uint16_t u16() {
    std::uint16_t value = 0;
    for (int i = 0; i < 2; ++i) {
      value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(u8()) << (8 * i)));
    }
    return value;
  }

  std::uint32_t u32() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(u8()) << (8 * i);
    }
    return value;
  }

  std::uint64_t u64() {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(u8()) << (8 * i);
    }
    return value;
  }

  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  bool boolean() { return u8() != 0; }

  std::string text() {
    const std::uint32_t length = u32();
    if (!ok_) {
      return {};
    }
    if (length > limits_.max_document_bytes || position_ + length > size_) {
      fail("snapshot text field exceeds the configured bound");
      return {};
    }
    std::string out(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return out;
  }

  std::vector<std::uint8_t> blob() {
    const std::uint64_t length = u64();
    if (!ok_) {
      return {};
    }
    if (length > limits_.max_snapshot_bytes || position_ + length > size_) {
      fail("snapshot blob exceeds the configured bound");
      return {};
    }
    std::vector<std::uint8_t> out(data_ + position_, data_ + position_ + static_cast<std::size_t>(length));
    position_ += static_cast<std::size_t>(length);
    return out;
  }

  std::array<std::uint64_t, 2> digest_id() {
    std::array<std::uint64_t, 2> value{0, 0};
    value[0] = u64();
    value[1] = u64();
    return value;
  }

  // Reads a count and checks it against a bound before any allocation happens.
  std::uint32_t count(std::size_t bound, const char* what) {
    const std::uint32_t value = u32();
    if (!ok_) {
      return 0;
    }
    if (value > bound) {
      fail(what);
      return 0;
    }
    return value;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  Limits limits_;
  std::size_t position_{0};
  bool ok_{true};
  std::string failure_{};
};

void write_generation(ByteWriter& writer, const GenerationVector& generation) {
  writer.u64(generation.epoch.value());
  writer.u64(generation.generation.value());
  writer.u64(generation.revision.value());
}

GenerationVector read_generation(ByteReader& reader) {
  GenerationVector generation;
  generation.epoch = Epoch(reader.u64());
  generation.generation = Generation(reader.u64());
  generation.revision = Revision(reader.u64());
  return generation;
}

void write_subject(ByteWriter& writer, const EvidenceSubject& subject) {
  writer.u8(static_cast<std::uint8_t>(subject.kind()));
  writer.text(subject.str());
}

EvidenceSubject read_subject(ByteReader& reader) {
  const auto kind = static_cast<SubjectKind>(reader.u8());
  const std::string text = reader.text();
  if (!reader.ok() || kind == SubjectKind::kNone) {
    return EvidenceSubject{};
  }
  auto parsed = parse_subject(text);
  if (!parsed.ok()) {
    reader.fail("snapshot contains an unparsable subject");
    return EvidenceSubject{};
  }
  return parsed.value();
}

void write_name_list(ByteWriter& writer, const std::vector<Name>& names, std::size_t bound) {
  writer.u32(static_cast<std::uint32_t>(names.size() > bound ? bound : names.size()));
  for (std::size_t i = 0; i < names.size() && i < bound; ++i) {
    writer.text(names[i].view());
  }
}

void write_metadata(ByteWriter& writer, const std::vector<std::pair<std::string, std::string>>& items,
                    std::size_t bound) {
  writer.u32(static_cast<std::uint32_t>(items.size() > bound ? bound : items.size()));
  for (std::size_t i = 0; i < items.size() && i < bound; ++i) {
    writer.text(items[i].first);
    writer.text(items[i].second);
  }
}

void write_evidence(ByteWriter& writer, const EvidenceRecord& record) {
  writer.digest_id(record.id.digest());
  writer.u16(static_cast<std::uint16_t>(record.kind));
  write_subject(writer, record.subject);
  writer.text(record.provenance.source.view());
  writer.u8(static_cast<std::uint8_t>(record.provenance.authority));
  writer.text(record.provenance.transport);
  writer.text(record.provenance.collector);
  writer.text(record.fence.source.view());
  writer.u64(record.fence.boot.value());
  writer.u64(record.fence.incarnation.value());
  write_generation(writer, record.fence.gen);
  writer.u64(record.fence.sequence.value());
  writer.i64(record.observed_at.unix_nanos());
  writer.i64(record.received_at.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(record.clock));
  std::uint64_t bits = 0;
  std::memcpy(&bits, &record.value.scalar, sizeof(bits));
  writer.u64(bits);
  writer.u8(static_cast<std::uint8_t>(record.value.unit));
  writer.u8(static_cast<std::uint8_t>(record.value.semantics));
  writer.i64(record.validity.nanos());
  writer.u8(static_cast<std::uint8_t>(record.completeness));
  writer.u8(static_cast<std::uint8_t>(record.support));
  writer.u8(record.recovered_from_snapshot ? 1u : 0u);
  writer.u8(record.retired ? 1u : 0u);
  writer.text(record.note);
  writer.u32(static_cast<std::uint32_t>(record.labels.size()));
  for (const Name& label : record.labels) {
    writer.text(label.view());
  }
  writer.u32(static_cast<std::uint32_t>(record.metadata.size()));
  for (const auto& entry : record.metadata) {
    writer.text(entry.first);
    writer.text(entry.second);
  }
}

void write_episode(ByteWriter& writer, const Episode& episode) {
  writer.digest_id(episode.id.digest());
  write_subject(writer, episode.key.scope);
  writer.text(episode.key.tenant.view());
  writer.u8(static_cast<std::uint8_t>(episode.key.mechanism));
  write_generation(writer, episode.key.generation);
  writer.text(episode.key.policy_version);
  writer.u8(static_cast<std::uint8_t>(episode.state));
  writer.u64(episode.revision.value());
  writer.u8(static_cast<std::uint8_t>(episode.current_severity));
  writer.u8(static_cast<std::uint8_t>(episode.peak_severity));
  writer.u32(episode.confidence);
  writer.i64(episode.first_seen.unix_nanos());
  writer.i64(episode.last_seen.unix_nanos());
  writer.i64(episode.last_updated.unix_nanos());
  writer.i64(episode.resolved_at.unix_nanos());
  writer.u64(episode.observations);
  writer.u32(static_cast<std::uint32_t>(episode.citations.size()));
  for (const EvidenceId& citation : episode.citations) {
    writer.digest_id(citation.digest());
  }
  writer.u32(static_cast<std::uint32_t>(episode.sources.size()));
  for (const SourceId& source : episode.sources) {
    writer.text(source.view());
  }
  writer.u32(static_cast<std::uint32_t>(episode.transitions.size()));
  for (const EpisodeTransition& transition : episode.transitions) {
    writer.u64(transition.revision.value());
    writer.u8(static_cast<std::uint8_t>(transition.kind));
    writer.i64(transition.at.unix_nanos());
    writer.u8(static_cast<std::uint8_t>(transition.severity_before));
    writer.u8(static_cast<std::uint8_t>(transition.severity_after));
    writer.u8(static_cast<std::uint8_t>(transition.verdict_before));
    writer.u8(static_cast<std::uint8_t>(transition.verdict_after));
    writer.u32(transition.confidence);
    writer.text(transition.cause);
    writer.u32(static_cast<std::uint32_t>(transition.citations.size()));
    for (const EvidenceId& citation : transition.citations) {
      writer.digest_id(citation.digest());
    }
  }
  writer.u32(static_cast<std::uint32_t>(episode.assessments.size()));
  for (const AssessmentDigest& digest : episode.assessments) {
    writer.i64(digest.at.unix_nanos());
    writer.u8(static_cast<std::uint8_t>(digest.verdict));
    writer.u32(digest.mechanisms);
    writer.u8(static_cast<std::uint8_t>(digest.severity));
    writer.u32(digest.confidence);
    writer.u32(static_cast<std::uint32_t>(digest.citation_count));
    writer.u8(digest.fresh_confirming_evidence ? 1u : 0u);
    writer.digest_id(digest.representative_citation.digest());
  }
  writer.u64(episode.transitions_dropped);
  writer.u64(episode.assessments_dropped);
  writer.u8(episode.history_truncated ? 1u : 0u);
}

std::vector<std::uint8_t> encode_topology_section(const Topology& topology) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  write_generation(writer, topology.generation());
  writer.text(topology.revision_label().view());

  writer.u32(static_cast<std::uint32_t>(topology.nodes().size()));
  for (const auto& entry : topology.nodes()) {
    writer.text(entry.second.id.view());
    writer.u8(static_cast<std::uint8_t>(entry.second.kind));
    write_name_list(writer, entry.second.labels, 64);
  }
  writer.u32(static_cast<std::uint32_t>(topology.ports().size()));
  for (const auto& entry : topology.ports()) {
    writer.text(entry.second.id.view());
    writer.text(entry.second.node.view());
    writer.u64(entry.second.speed_bps);
  }
  writer.u32(static_cast<std::uint32_t>(topology.links().size()));
  for (const auto& entry : topology.links()) {
    writer.text(entry.second.id.view());
    writer.text(entry.second.endpoint_a.view());
    writer.text(entry.second.endpoint_b.view());
    writer.u64(entry.second.capacity_bps);
  }
  writer.u32(static_cast<std::uint32_t>(topology.queues().size()));
  for (const auto& entry : topology.queues()) {
    writer.text(entry.second.id.view());
    writer.text(entry.second.link.view());
    writer.u32(entry.second.index);
    writer.u32(entry.second.min_share_bp);
    writer.u32(entry.second.max_share_bp);
  }
  writer.u32(static_cast<std::uint32_t>(topology.buffers().size()));
  for (const auto& entry : topology.buffers()) {
    writer.text(entry.second.id.view());
    writer.text(entry.second.port.view());
    writer.u32(entry.second.pool);
    writer.u64(entry.second.cells);
  }
  writer.u32(static_cast<std::uint32_t>(topology.paths().size()));
  for (const auto& entry : topology.paths()) {
    writer.text(entry.second.id.view());
    writer.u32(static_cast<std::uint32_t>(entry.second.hops.size()));
    for (const LinkId& hop : entry.second.hops) {
      writer.text(hop.view());
    }
  }
  writer.u32(static_cast<std::uint32_t>(topology.flows().size()));
  for (const auto& entry : topology.flows()) {
    writer.text(entry.second.id.view());
    writer.text(entry.second.path.view());
    writer.text(entry.second.tenant.view());
    writer.text(entry.second.traffic_class.view());
  }
  return out;
}

Result<Topology> decode_topology_section(const std::vector<std::uint8_t>& bytes, const Limits& limits) {
  ByteReader reader(bytes.data(), bytes.size(), limits);
  TopologyBuilder builder;
  builder.set_limits(limits);
  builder.set_generation(read_generation(reader));
  const std::string label = reader.text();
  if (!label.empty()) {
    auto parsed = Name::parse(label);
    if (!parsed.ok()) {
      return parsed.error();
    }
    builder.set_revision_label(parsed.value());
  }

  const std::uint32_t node_count = reader.count(limits.max_nodes, "node count exceeds the limit");
  for (std::uint32_t i = 0; i < node_count && reader.ok(); ++i) {
    Node node;
    auto id = NodeId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    node.id = id.value();
    node.kind = static_cast<NodeKind>(reader.u8());
    const std::uint32_t labels = reader.count(limits.max_labels, "label count exceeds the limit");
    for (std::uint32_t l = 0; l < labels && reader.ok(); ++l) {
      auto label_value = Name::parse(reader.text());
      if (!label_value.ok()) {
        return label_value.error();
      }
      node.labels.push_back(label_value.value());
    }
    const Status added = builder.add_node(node);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t port_count = reader.count(limits.max_ports, "port count exceeds the limit");
  for (std::uint32_t i = 0; i < port_count && reader.ok(); ++i) {
    Port port;
    auto id = PortId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    port.id = id.value();
    auto node = NodeId::parse(reader.text());
    if (!node.ok()) {
      return node.error();
    }
    port.node = node.value();
    port.speed_bps = reader.u64();
    const Status added = builder.add_port(port);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t link_count = reader.count(limits.max_links, "link count exceeds the limit");
  for (std::uint32_t i = 0; i < link_count && reader.ok(); ++i) {
    Link link;
    auto id = LinkId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    link.id = id.value();
    auto endpoint_a = PortId::parse(reader.text());
    if (!endpoint_a.ok()) {
      return endpoint_a.error();
    }
    link.endpoint_a = endpoint_a.value();
    auto endpoint_b = PortId::parse(reader.text());
    if (!endpoint_b.ok()) {
      return endpoint_b.error();
    }
    link.endpoint_b = endpoint_b.value();
    link.capacity_bps = reader.u64();
    const Status added = builder.add_link(link);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t queue_count = reader.count(limits.max_queues, "queue count exceeds the limit");
  for (std::uint32_t i = 0; i < queue_count && reader.ok(); ++i) {
    Queue queue;
    auto id = QueueId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    queue.id = id.value();
    auto link = LinkId::parse(reader.text());
    if (!link.ok()) {
      return link.error();
    }
    queue.link = link.value();
    queue.index = reader.u32();
    queue.min_share_bp = reader.u32();
    queue.max_share_bp = reader.u32();
    const Status added = builder.add_queue(queue);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t buffer_count =
      reader.count(limits.max_buffers, "buffer count exceeds the limit");
  for (std::uint32_t i = 0; i < buffer_count && reader.ok(); ++i) {
    Buffer buffer;
    auto id = BufferId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    buffer.id = id.value();
    auto port = PortId::parse(reader.text());
    if (!port.ok()) {
      return port.error();
    }
    buffer.port = port.value();
    buffer.pool = reader.u32();
    buffer.cells = reader.u64();
    const Status added = builder.add_buffer(buffer);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t path_count = reader.count(limits.max_paths, "path count exceeds the limit");
  for (std::uint32_t i = 0; i < path_count && reader.ok(); ++i) {
    Path path;
    auto id = PathId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    path.id = id.value();
    const std::uint32_t hops = reader.count(limits.max_path_hops, "hop count exceeds the limit");
    for (std::uint32_t h = 0; h < hops && reader.ok(); ++h) {
      auto hop = LinkId::parse(reader.text());
      if (!hop.ok()) {
        return hop.error();
      }
      path.hops.push_back(hop.value());
    }
    const Status added = builder.add_path(path);
    if (!added.ok()) {
      return added.error();
    }
  }

  const std::uint32_t flow_count = reader.count(limits.max_flows, "flow count exceeds the limit");
  for (std::uint32_t i = 0; i < flow_count && reader.ok(); ++i) {
    Flow flow;
    auto id = FlowId::parse(reader.text());
    if (!id.ok()) {
      return id.error();
    }
    flow.id = id.value();
    auto path = PathId::parse(reader.text());
    if (!path.ok()) {
      return path.error();
    }
    flow.path = path.value();
    auto tenant = TenantId::parse(reader.text());
    if (!tenant.ok()) {
      return tenant.error();
    }
    flow.tenant = tenant.value();
    auto traffic_class = ClassId::parse(reader.text());
    if (!traffic_class.ok()) {
      return traffic_class.error();
    }
    flow.traffic_class = traffic_class.value();
    const Status added = builder.add_flow(flow);
    if (!added.ok()) {
      return added.error();
    }
  }

  if (!reader.ok()) {
    return make_error(ErrorCode::kIntegrityFailure, "topology section is malformed",
                      reader.failure());
  }
  if (reader.remaining() != 0) {
    return make_error(ErrorCode::kIntegrityFailure, "topology section has trailing bytes");
  }
  return builder.build(limits);
}

}  // namespace

Result<std::vector<std::uint8_t>> encode_snapshot(const SnapshotContent& content,
                                                  const Limits& limits) {
  const Status limits_valid = limits.validate();
  if (!limits_valid.ok()) {
    return limits_valid.error();
  }
  // The encoding stamps the digests it can derive itself: a caller cannot desynchronise the
  // header from the payload it describes.
  SnapshotContent staged = content;
  staged.metadata.limits_digest = limits.digest();
  staged.metadata.topology_digest = staged.topology.digest();

  std::vector<std::uint8_t> topology_section = encode_topology_section(staged.topology);

  std::vector<std::uint8_t> episode_section;
  {
    ByteWriter writer(episode_section);
    const std::size_t count =
        std::min(staged.episodes.size(), limits.max_snapshot_episodes);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
      write_episode(writer, staged.episodes[i]);
    }
  }

  std::vector<std::uint8_t> evidence_section;
  {
    ByteWriter writer(evidence_section);
    const std::size_t count = std::min(staged.evidence.size(), limits.max_snapshot_evidence);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
      write_evidence(writer, staged.evidence[i]);
    }
  }

  std::vector<std::uint8_t> metadata_section;
  {
    ByteWriter writer(metadata_section);
    writer.text(staged.metadata.policy_version);
    writer.u64(staged.metadata.policy_digest);
    writer.u64(staged.metadata.limits_digest);
    writer.u64(staged.metadata.topology_digest);
    writer.i64(staged.metadata.created_at.unix_nanos());
    writer.u64(staged.metadata.runtime_instance);
    writer.u64(staged.metadata.accepted_total);
    writer.u64(staged.metadata.rejected_total);
    write_generation(writer, staged.metadata.generation);
    writer.text(staged.metadata.producer_version);
  }

  struct PendingSection {
    std::uint32_t id;
    const std::vector<std::uint8_t>* bytes;
  };
  const PendingSection pending[] = {
      {static_cast<std::uint32_t>(SnapshotSectionId::kMetadata), &metadata_section},
      {static_cast<std::uint32_t>(SnapshotSectionId::kTopology), &topology_section},
      {static_cast<std::uint32_t>(SnapshotSectionId::kEpisodes), &episode_section},
      {static_cast<std::uint32_t>(SnapshotSectionId::kEvidence), &evidence_section},
  };
  const std::size_t section_count = sizeof(pending) / sizeof(pending[0]);

  std::uint64_t payload_bytes = 0;
  for (const PendingSection& section : pending) {
    std::uint64_t next = 0;
    if (!checked_add<std::uint64_t>(payload_bytes, section.bytes->size(), next)) {
      return make_error(ErrorCode::kArithmeticOverflow, "snapshot payload size overflow");
    }
    payload_bytes = next;
  }
  if (payload_bytes > limits.max_snapshot_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "snapshot exceeds the configured byte bound",
                      std::to_string(payload_bytes));
  }

  std::vector<std::uint8_t> payload;
  payload.reserve(static_cast<std::size_t>(payload_bytes));
  std::vector<SnapshotSection> table;
  for (const PendingSection& section : pending) {
    SnapshotSection entry;
    entry.id = section.id;
    entry.offset = payload.size();
    entry.length = section.bytes->size();
    entry.crc64 = section.bytes->empty()
                      ? crc64(std::string_view{})
                      : crc64(std::string_view(reinterpret_cast<const char*>(section.bytes->data()),
                                               section.bytes->size()));
    table.push_back(entry);
    payload.insert(payload.end(), section.bytes->begin(), section.bytes->end());
  }

  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  for (const char c : kSnapshotMagic) {
    writer.u8(static_cast<std::uint8_t>(c));
  }
  writer.u32(kSnapshotFormatMajor);
  writer.u32(kSnapshotFormatMinor);
  writer.u64(staged.metadata.limits_digest);
  writer.u64(staged.metadata.policy_digest);
  writer.u64(staged.metadata.topology_digest);
  writer.i64(staged.metadata.created_at.unix_nanos());
  writer.u32(static_cast<std::uint32_t>(section_count));
  writer.u64(payload_bytes);
  const std::uint64_t payload_crc =
      payload.empty() ? crc64(std::string_view{})
                      : crc64(std::string_view(reinterpret_cast<const char*>(payload.data()),
                                               payload.size()));
  writer.u64(payload_crc);
  for (const SnapshotSection& section : table) {
    writer.u32(section.id);
    writer.u64(section.offset);
    writer.u64(section.length);
    writer.u64(section.crc64);
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Result<SnapshotContent> decode_snapshot(const std::vector<std::uint8_t>& bytes,
                                        const Limits& limits,
                                        const ClassificationPolicy& policy) {
  if (bytes.size() < kSnapshotHeaderBytes) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot is shorter than its header");
  }
  ByteReader reader(bytes.data(), bytes.size(), limits);
  for (const char expected : kSnapshotMagic) {
    if (reader.u8() != static_cast<std::uint8_t>(expected)) {
      return make_error(ErrorCode::kIntegrityFailure, "snapshot magic does not match");
    }
  }
  const std::uint32_t major = reader.u32();
  const std::uint32_t minor = reader.u32();
  if (major != kSnapshotFormatMajor) {
    return make_error(ErrorCode::kVersionMismatch, "snapshot format major version differs",
                      "found=" + std::to_string(major) +
                          " expected=" + std::to_string(kSnapshotFormatMajor));
  }
  if (minor > kSnapshotFormatMinor) {
    return make_error(ErrorCode::kVersionMismatch, "snapshot was written by a newer minor format",
                      "found=" + std::to_string(minor) +
                          " supported=" + std::to_string(kSnapshotFormatMinor));
  }
  const std::uint64_t limits_digest = reader.u64();
  const std::uint64_t policy_digest = reader.u64();
  const std::uint64_t topology_digest = reader.u64();
  const std::int64_t created_at = reader.i64();
  const std::uint32_t section_count =
      reader.count(limits.max_snapshot_sections, "section count exceeds the limit");
  const std::uint64_t payload_bytes = reader.u64();
  const std::uint64_t payload_crc = reader.u64();
  if (!reader.ok()) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot header is malformed", reader.failure());
  }
  if (payload_bytes > limits.max_snapshot_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "snapshot payload exceeds the configured bound",
                      std::to_string(payload_bytes));
  }
  if (limits_digest != limits.digest()) {
    return make_error(ErrorCode::kVersionMismatch,
                      "snapshot was written under different resource limits");
  }
  if (policy_digest != policy.digest()) {
    return make_error(ErrorCode::kVersionMismatch,
                      "snapshot was written under a different classification policy");
  }

  std::vector<SnapshotSection> table;
  for (std::uint32_t i = 0; i < section_count && reader.ok(); ++i) {
    SnapshotSection section;
    section.id = reader.u32();
    section.offset = reader.u64();
    section.length = reader.u64();
    section.crc64 = reader.u64();
    table.push_back(section);
  }
  if (!reader.ok()) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot section table is malformed",
                      reader.failure());
  }
  const std::size_t payload_offset = reader.position();
  if (payload_offset + payload_bytes != bytes.size()) {
    return make_error(ErrorCode::kIntegrityFailure,
                      "snapshot length does not match the declared payload size");
  }
  const std::string_view payload(reinterpret_cast<const char*>(bytes.data() + payload_offset),
                                 static_cast<std::size_t>(payload_bytes));
  if (crc64(payload) != payload_crc) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot payload checksum mismatch");
  }

  SnapshotContent content;
  content.metadata.created_at = Timestamp(created_at);
  content.metadata.limits_digest = limits_digest;
  content.metadata.policy_digest = policy_digest;
  content.metadata.topology_digest = topology_digest;

  std::map<std::uint32_t, std::vector<std::uint8_t>> sections;
  for (const SnapshotSection& section : table) {
    std::uint64_t end = 0;
    if (!checked_add<std::uint64_t>(section.offset, section.length, end) || end > payload_bytes) {
      return make_error(ErrorCode::kIntegrityFailure, "snapshot section extent is out of range");
    }
    const std::string_view slice(payload.data() + section.offset,
                                 static_cast<std::size_t>(section.length));
    const std::uint64_t actual = section.length == 0 ? crc64(std::string_view{}) : crc64(slice);
    if (actual != section.crc64) {
      return make_error(ErrorCode::kIntegrityFailure, "snapshot section checksum mismatch",
                        "section=" + std::to_string(section.id));
    }
    if (sections.find(section.id) != sections.end()) {
      return make_error(ErrorCode::kIntegrityFailure, "snapshot contains a duplicate section",
                        "section=" + std::to_string(section.id));
    }
    sections.emplace(section.id,
                     std::vector<std::uint8_t>(slice.begin(), slice.end()));
  }

  const auto metadata_it =
      sections.find(static_cast<std::uint32_t>(SnapshotSectionId::kMetadata));
  if (metadata_it == sections.end()) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot has no metadata section");
  }
  {
    ByteReader meta(metadata_it->second.data(), metadata_it->second.size(), limits);
    content.metadata.policy_version = meta.text();
    content.metadata.policy_digest = meta.u64();
    content.metadata.limits_digest = meta.u64();
    content.metadata.topology_digest = meta.u64();
    content.metadata.created_at = Timestamp(meta.i64());
    content.metadata.runtime_instance = meta.u64();
    content.metadata.accepted_total = meta.u64();
    content.metadata.rejected_total = meta.u64();
    content.metadata.generation = read_generation(meta);
    content.metadata.producer_version = meta.text();
    if (!meta.ok()) {
      return make_error(ErrorCode::kIntegrityFailure, "metadata section is malformed",
                        meta.failure());
    }
    if (content.metadata.policy_version != policy.version) {
      return make_error(ErrorCode::kVersionMismatch,
                        "snapshot policy version differs from the active policy",
                        content.metadata.policy_version + " != " + policy.version);
    }
  }

  const auto topology_it = sections.find(static_cast<std::uint32_t>(SnapshotSectionId::kTopology));
  if (topology_it == sections.end()) {
    return make_error(ErrorCode::kIntegrityFailure, "snapshot has no topology section");
  }
  auto topology = decode_topology_section(topology_it->second, limits);
  if (!topology.ok()) {
    return topology.error();
  }
  content.topology = std::move(topology.value());
  if (content.topology.digest() != topology_digest) {
    return make_error(ErrorCode::kIntegrityFailure, "topology digest does not match the header");
  }

  const auto episodes_it = sections.find(static_cast<std::uint32_t>(SnapshotSectionId::kEpisodes));
  if (episodes_it != sections.end()) {
    ByteReader episode_reader(episodes_it->second.data(), episodes_it->second.size(), limits);
    const std::uint32_t count =
        episode_reader.count(limits.max_snapshot_episodes, "episode count exceeds the limit");
    for (std::uint32_t i = 0; i < count && episode_reader.ok(); ++i) {
      Episode episode;
      episode.id = EpisodeId::from_digest(episode_reader.digest_id());
      episode.key.scope = read_subject(episode_reader);
      // An empty tenant means "not attributed", which is a valid and distinct state.
      const std::string tenant_text = episode_reader.text();
      if (!tenant_text.empty()) {
        auto tenant = TenantId::parse(tenant_text);
        if (!tenant.ok()) {
          return tenant.error();
        }
        episode.key.tenant = tenant.value();
      }
      episode.key.mechanism = static_cast<Mechanism>(episode_reader.u8());
      episode.key.generation = read_generation(episode_reader);
      episode.key.policy_version = episode_reader.text();
      episode.state = static_cast<EpisodeState>(episode_reader.u8());
      episode.revision = Revision(episode_reader.u64());
      episode.current_severity = static_cast<Severity>(episode_reader.u8());
      episode.peak_severity = static_cast<Severity>(episode_reader.u8());
      episode.confidence = episode_reader.u32();
      episode.first_seen = Timestamp(episode_reader.i64());
      episode.last_seen = Timestamp(episode_reader.i64());
      episode.last_updated = Timestamp(episode_reader.i64());
      episode.resolved_at = Timestamp(episode_reader.i64());
      episode.observations = episode_reader.u64();
      const std::uint32_t citation_count =
          episode_reader.count(limits.max_citations, "citation count exceeds the limit");
      for (std::uint32_t c = 0; c < citation_count && episode_reader.ok(); ++c) {
        episode.citations.push_back(EvidenceId::from_digest(episode_reader.digest_id()));
      }
      const std::uint32_t source_count =
          episode_reader.count(limits.max_sources, "source count exceeds the limit");
      for (std::uint32_t s = 0; s < source_count && episode_reader.ok(); ++s) {
        auto source = SourceId::parse(episode_reader.text());
        if (!source.ok()) {
          return source.error();
        }
        episode.sources.push_back(source.value());
      }
      const std::uint32_t transition_count = episode_reader.count(
          limits.max_episode_transitions, "transition count exceeds the limit");
      for (std::uint32_t t = 0; t < transition_count && episode_reader.ok(); ++t) {
        EpisodeTransition transition;
        transition.revision = Revision(episode_reader.u64());
        transition.kind = static_cast<EpisodeTransitionKind>(episode_reader.u8());
        transition.at = Timestamp(episode_reader.i64());
        transition.severity_before = static_cast<Severity>(episode_reader.u8());
        transition.severity_after = static_cast<Severity>(episode_reader.u8());
        transition.verdict_before = static_cast<Verdict>(episode_reader.u8());
        transition.verdict_after = static_cast<Verdict>(episode_reader.u8());
        transition.confidence = episode_reader.u32();
        transition.cause = episode_reader.text();
        const std::uint32_t citations =
            episode_reader.count(limits.max_citations, "transition citation count exceeds limit");
        for (std::uint32_t c = 0; c < citations && episode_reader.ok(); ++c) {
          transition.citations.push_back(EvidenceId::from_digest(episode_reader.digest_id()));
        }
        episode.transitions.push_back(std::move(transition));
      }
      const std::uint32_t assessment_count =
          episode_reader.count(limits.max_episode_assessments, "assessment count exceeds limit");
      for (std::uint32_t a = 0; a < assessment_count && episode_reader.ok(); ++a) {
        AssessmentDigest digest;
        digest.at = Timestamp(episode_reader.i64());
        digest.verdict = static_cast<Verdict>(episode_reader.u8());
        digest.mechanisms = episode_reader.u32();
        digest.severity = static_cast<Severity>(episode_reader.u8());
        digest.confidence = episode_reader.u32();
        digest.citation_count = episode_reader.u32();
        digest.fresh_confirming_evidence = episode_reader.boolean();
        digest.representative_citation =
            EvidenceId::from_digest(episode_reader.digest_id());
        episode.assessments.push_back(digest);
      }
      episode.transitions_dropped = episode_reader.u64();
      episode.assessments_dropped = episode_reader.u64();
      episode.history_truncated = episode_reader.boolean();
      if (!episode_reader.ok()) {
        return make_error(ErrorCode::kIntegrityFailure, "episode section is malformed",
                          episode_reader.failure());
      }
      const EpisodeId expected = compute_episode_id(episode.key);
      if (expected != episode.id) {
        return make_error(ErrorCode::kIntegrityFailure,
                          "episode identity does not match its key", episode.id.str());
      }
      content.episodes.push_back(std::move(episode));
    }
    if (!episode_reader.ok()) {
      return make_error(ErrorCode::kIntegrityFailure, "episode section is malformed",
                        episode_reader.failure());
    }
    if (episode_reader.remaining() != 0) {
      return make_error(ErrorCode::kIntegrityFailure, "episode section has trailing bytes");
    }
  }

  const auto evidence_it = sections.find(static_cast<std::uint32_t>(SnapshotSectionId::kEvidence));
  if (evidence_it != sections.end()) {
    ByteReader evidence_reader(evidence_it->second.data(), evidence_it->second.size(), limits);
    const std::uint32_t raw_count = evidence_reader.u32();
    if (raw_count > limits.max_snapshot_evidence) {
      content.evidence_truncated = true;
    }
    const std::uint32_t count =
        raw_count > limits.max_snapshot_evidence ? 0 : raw_count;
    for (std::uint32_t i = 0; i < count && evidence_reader.ok(); ++i) {
      EvidenceRecord record;
      record.id = EvidenceId::from_digest(evidence_reader.digest_id());
      record.kind = static_cast<EvidenceKind>(evidence_reader.u16());
      record.subject = read_subject(evidence_reader);
      auto source = SourceId::parse(evidence_reader.text());
      if (!source.ok()) {
        return source.error();
      }
      record.provenance.source = source.value();
      record.provenance.authority = static_cast<AuthorityLevel>(evidence_reader.u8());
      record.provenance.transport = evidence_reader.text();
      record.provenance.collector = evidence_reader.text();
      auto fence_source = SourceId::parse(evidence_reader.text());
      if (!fence_source.ok()) {
        return fence_source.error();
      }
      record.fence.source = fence_source.value();
      record.fence.boot = BootId(evidence_reader.u64());
      record.fence.incarnation = Incarnation(evidence_reader.u64());
      record.fence.gen = read_generation(evidence_reader);
      record.fence.sequence = Sequence(evidence_reader.u64());
      record.observed_at = Timestamp(evidence_reader.i64());
      record.received_at = Timestamp(evidence_reader.i64());
      record.clock = static_cast<ClockDomain>(evidence_reader.u8());
      std::uint64_t bits = evidence_reader.u64();
      double scalar = 0.0;
      std::memcpy(&scalar, &bits, sizeof(scalar));
      record.value.scalar = scalar;
      record.value.unit = static_cast<ObservationUnit>(evidence_reader.u8());
      record.value.semantics = static_cast<ValueSemantics>(evidence_reader.u8());
      record.validity = Duration(evidence_reader.i64());
      record.completeness = static_cast<Completeness>(evidence_reader.u8());
      record.support = static_cast<Support>(evidence_reader.u8());
      record.recovered_from_snapshot = evidence_reader.boolean();
      record.retired = evidence_reader.boolean();
      record.note = evidence_reader.text();
      const std::uint32_t label_count =
          evidence_reader.count(limits.max_labels, "label count exceeds the limit");
      for (std::uint32_t l = 0; l < label_count && evidence_reader.ok(); ++l) {
        auto label = Name::parse(evidence_reader.text());
        if (!label.ok()) {
          return label.error();
        }
        record.labels.push_back(label.value());
      }
      const std::uint32_t metadata_count =
          evidence_reader.count(limits.max_metadata_entries, "metadata count exceeds the limit");
      for (std::uint32_t m = 0; m < metadata_count && evidence_reader.ok(); ++m) {
        std::string key = evidence_reader.text();
        std::string value = evidence_reader.text();
        if (key.size() > limits.max_metadata_key_bytes ||
            value.size() > limits.max_metadata_value_bytes) {
          return make_error(ErrorCode::kLimitExceeded,
                            "metadata entry exceeds the configured bound");
        }
        record.metadata.emplace_back(std::move(key), std::move(value));
      }
      if (!evidence_reader.ok()) {
        return make_error(ErrorCode::kIntegrityFailure, "evidence section is malformed",
                          evidence_reader.failure());
      }
      const EvidenceId expected = compute_evidence_id(record);
      if (expected != record.id) {
        return make_error(ErrorCode::kIntegrityFailure,
                          "evidence identity does not match its content", record.id.str());
      }
      content.evidence.push_back(std::move(record));
    }
    if (!evidence_reader.ok()) {
      return make_error(ErrorCode::kIntegrityFailure, "evidence section is malformed",
                        evidence_reader.failure());
    }
    if (evidence_reader.remaining() != 0) {
      return make_error(ErrorCode::kIntegrityFailure, "evidence section has trailing bytes");
    }
  }

  content.metadata.generation = content.topology.generation();
  return content;
}

}  // namespace congestion
