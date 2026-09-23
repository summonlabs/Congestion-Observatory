// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "co_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "congestion/core/json.hpp"
#include "congestion/transport/server.hpp"

namespace congestion::tools {
namespace {

Result<std::int64_t> parse_int(std::string_view text, const char* what) {
  if (text.empty()) {
    return make_error(ErrorCode::kInvalidArgument, std::string("missing ") + what);
  }
  std::int64_t value = 0;
  bool negative = false;
  std::size_t index = 0;
  if (text.front() == '-') {
    negative = true;
    index = 1;
  }
  if (index >= text.size()) {
    return make_error(ErrorCode::kInvalidArgument, std::string("invalid ") + what);
  }
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::kInvalidArgument, std::string("invalid ") + what,
                        std::string(text));
    }
    if (value > (INT64_MAX - (c - '0')) / 10) {
      return make_error(ErrorCode::kArithmeticOverflow, std::string(what) + " is out of range");
    }
    value = value * 10 + (c - '0');
  }
  return negative ? -value : value;
}

Result<std::string> json_string(const JsonValue& value, const char* what) {
  if (!value.is_string()) {
    return make_error(ErrorCode::kInvalidArgument, std::string("field must be a string: ") + what);
  }
  return value.as_string();
}

std::string string_or(const JsonValue& object, const char* key, const char* fallback) {
  const JsonValue* value = object.member(key);
  if (value == nullptr || !value->is_string()) {
    return fallback;
  }
  return value->as_string();
}

std::uint64_t number_or(const JsonValue& object, const char* key, std::uint64_t fallback) {
  const JsonValue* value = object.member(key);
  if (value == nullptr || (!value->is_int() && !value->is_double())) {
    return fallback;
  }
  const std::int64_t raw = value->as_int(static_cast<std::int64_t>(fallback));
  return raw < 0 ? fallback : static_cast<std::uint64_t>(raw);
}

}  // namespace

Result<Duration> parse_duration(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "empty duration");
  }
  std::size_t index = 0;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    ++index;
  }
  auto number = parse_int(text.substr(0, index), "duration value");
  if (!number.ok()) {
    return number.error();
  }
  const std::string_view unit = text.substr(index);
  std::int64_t scale = 1000000000;  // default: seconds
  if (unit.empty() || unit == "s") {
    scale = 1000000000;
  } else if (unit == "ms") {
    scale = 1000000;
  } else if (unit == "us") {
    scale = 1000;
  } else if (unit == "ns") {
    scale = 1;
  } else if (unit == "m") {
    scale = 60LL * 1000000000LL;
  } else if (unit == "h") {
    scale = 3600LL * 1000000000LL;
  } else {
    return make_error(ErrorCode::kInvalidArgument, "unknown duration unit", std::string(unit));
  }
  if (number.value() > INT64_MAX / scale) {
    return make_error(ErrorCode::kArithmeticOverflow, "duration is out of range");
  }
  return Duration(number.value() * scale);
}

Result<Timestamp> parse_timestamp(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "empty timestamp");
  }
  if (text.front() == '@') {
    auto seconds = parse_int(text.substr(1), "epoch seconds");
    if (!seconds.ok()) {
      return seconds.error();
    }
    return Timestamp::from_unix_seconds(seconds.value());
  }
  return Timestamp::from_iso8601(text);
}

Result<std::string> read_text_file(const std::string& path, std::size_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return make_error(ErrorCode::kNotFound, "file is not readable", path);
  }
  if (size > max_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "file exceeds the configured bound", path);
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::kIoError, "file could not be opened", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Result<Topology> load_topology_document(std::string_view text, const Limits& limits) {
  auto document = parse_json(text, limits);
  if (!document.ok()) {
    return document.error();
  }
  if (!document.value().is_object()) {
    return make_error(ErrorCode::kInvalidArgument, "topology document must be a JSON object");
  }
  const JsonValue& root = document.value();
  TopologyBuilder builder;
  builder.set_limits(limits);

  GenerationVector generation;
  if (const JsonValue* generation_value = root.member("generation")) {
    if (generation_value->is_object()) {
      generation.epoch = Epoch(number_or(*generation_value, "epoch", 1));
      generation.generation = Generation(number_or(*generation_value, "generation", 1));
      generation.revision = Revision(number_or(*generation_value, "revision", 1));
    } else if (generation_value->is_string()) {
      // "epoch/generation/revision"
      const std::string text_value = generation_value->as_string();
      const std::size_t first = text_value.find('/');
      const std::size_t second = first == std::string::npos ? std::string::npos
                                                            : text_value.find('/', first + 1);
      if (first == std::string::npos || second == std::string::npos) {
        return make_error(ErrorCode::kInvalidArgument,
                          "generation string must be epoch/generation/revision", text_value);
      }
      auto epoch = parse_int(text_value.substr(0, first), "epoch");
      if (!epoch.ok()) return epoch.error();
      auto gen = parse_int(text_value.substr(first + 1, second - first - 1), "generation");
      if (!gen.ok()) return gen.error();
      auto revision = parse_int(text_value.substr(second + 1), "revision");
      if (!revision.ok()) return revision.error();
      if (epoch.value() < 0 || gen.value() < 0 || revision.value() < 0) {
        return make_error(ErrorCode::kInvalidArgument, "generation components must not be negative");
      }
      generation.epoch = Epoch(static_cast<std::uint64_t>(epoch.value()));
      generation.generation = Generation(static_cast<std::uint64_t>(gen.value()));
      generation.revision = Revision(static_cast<std::uint64_t>(revision.value()));
    } else {
      return make_error(ErrorCode::kInvalidArgument, "generation must be an object or a string");
    }
  }
  builder.set_generation(generation);
  const std::string label = string_or(root, "revision_label", "");
  if (!label.empty()) {
    auto parsed = Name::parse(label);
    if (!parsed.ok()) {
      return parsed.error();
    }
    builder.set_revision_label(parsed.value());
  }

  const auto each = [&](const char* key, const std::function<Status(const JsonValue&)>& handler) -> Status {
    const JsonValue* array = root.member(key);
    if (array == nullptr) {
      return Status{};
    }
    if (!array->is_array()) {
      return Status(make_error(ErrorCode::kInvalidArgument,
                               std::string("topology field must be an array: ") + key));
    }
    if (array->as_array().size() > 100000) {
      return Status(make_error(ErrorCode::kLimitExceeded,
                               std::string("topology array is too large: ") + key));
    }
    for (const JsonValue& item : array->as_array()) {
      if (!item.is_object()) {
        return Status(make_error(ErrorCode::kInvalidArgument,
                                 std::string("topology entry must be an object: ") + key));
      }
      const Status status = handler(item);
      if (!status.ok()) {
        return status;
      }
    }
    return Status{};
  };

  Status status = each("nodes", [&](const JsonValue& item) -> Status {
    auto id = NodeId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    Node node;
    node.id = id.value();
    auto kind = node_kind_from_string(string_or(item, "kind", "unknown"));
    if (!kind.ok()) return kind.error();
    node.kind = kind.value();
    if (const JsonValue* labels = item.member("labels")) {
      if (labels->is_array()) {
        for (const JsonValue& label : labels->as_array()) {
          auto parsed = Name::parse(label.as_string());
          if (!parsed.ok()) return parsed.error();
          node.labels.push_back(parsed.value());
        }
      }
    }
    return builder.add_node(node);
  });
  if (!status.ok()) return status.error();

  status = each("ports", [&](const JsonValue& item) -> Status {
    auto id = PortId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    auto node = NodeId::parse(string_or(item, "node", ""));
    if (!node.ok()) return node.error();
    Port port;
    port.id = id.value();
    port.node = node.value();
    port.speed_bps = number_or(item, "speed_bps", 0);
    return builder.add_port(port);
  });
  if (!status.ok()) return status.error();

  status = each("links", [&](const JsonValue& item) -> Status {
    auto id = LinkId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    auto a = PortId::parse(string_or(item, "endpoint_a", ""));
    if (!a.ok()) return a.error();
    auto b = PortId::parse(string_or(item, "endpoint_b", ""));
    if (!b.ok()) return b.error();
    Link link;
    link.id = id.value();
    link.endpoint_a = a.value();
    link.endpoint_b = b.value();
    link.capacity_bps = number_or(item, "capacity_bps", 0);
    return builder.add_link(link);
  });
  if (!status.ok()) return status.error();

  status = each("queues", [&](const JsonValue& item) -> Status {
    auto id = QueueId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    auto link = LinkId::parse(string_or(item, "link", ""));
    if (!link.ok()) return link.error();
    Queue queue;
    queue.id = id.value();
    queue.link = link.value();
    queue.index = static_cast<std::uint32_t>(number_or(item, "index", 0));
    queue.min_share_bp = static_cast<std::uint32_t>(number_or(item, "min_share_bp", 0));
    queue.max_share_bp = static_cast<std::uint32_t>(number_or(item, "max_share_bp", 10000));
    return builder.add_queue(queue);
  });
  if (!status.ok()) return status.error();

  status = each("buffers", [&](const JsonValue& item) -> Status {
    auto id = BufferId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    auto port = PortId::parse(string_or(item, "port", ""));
    if (!port.ok()) return port.error();
    Buffer buffer;
    buffer.id = id.value();
    buffer.port = port.value();
    buffer.pool = static_cast<std::uint32_t>(number_or(item, "pool", 0));
    buffer.cells = number_or(item, "cells", 0);
    return builder.add_buffer(buffer);
  });
  if (!status.ok()) return status.error();

  status = each("paths", [&](const JsonValue& item) -> Status {
    auto id = PathId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    Path path;
    path.id = id.value();
    const JsonValue* hops = item.member("hops");
    if (hops == nullptr || !hops->is_array()) {
      return Status(make_error(ErrorCode::kInvalidArgument, "path requires a hops array"));
    }
    for (const JsonValue& hop : hops->as_array()) {
      auto parsed = LinkId::parse(hop.as_string());
      if (!parsed.ok()) return parsed.error();
      path.hops.push_back(parsed.value());
    }
    return builder.add_path(path);
  });
  if (!status.ok()) return status.error();

  status = each("flows", [&](const JsonValue& item) -> Status {
    auto id = FlowId::parse(string_or(item, "id", ""));
    if (!id.ok()) return id.error();
    auto path = PathId::parse(string_or(item, "path", ""));
    if (!path.ok()) return path.error();
    auto tenant = TenantId::parse(string_or(item, "tenant", "unattributed"));
    if (!tenant.ok()) return tenant.error();
    auto traffic_class = ClassId::parse(string_or(item, "class", "default"));
    if (!traffic_class.ok()) return traffic_class.error();
    Flow flow;
    flow.id = id.value();
    flow.path = path.value();
    flow.tenant = tenant.value();
    flow.traffic_class = traffic_class.value();
    return builder.add_flow(flow);
  });
  if (!status.ok()) return status.error();

  return builder.build(limits);
}

Result<Topology> load_topology_file(const std::string& path, const Limits& limits) {
  auto text = read_text_file(path, limits.max_document_bytes);
  if (!text.ok()) {
    return text.error();
  }
  return load_topology_document(text.value(), limits);
}

Result<std::vector<EvidenceRecord>> load_evidence_document(std::string_view text,
                                                           const Limits& limits) {
  auto document = parse_json(text, limits);
  if (!document.ok()) {
    return document.error();
  }
  std::vector<const JsonValue*> entries;
  if (document.value().is_array()) {
    for (const JsonValue& item : document.value().as_array()) {
      entries.push_back(&item);
    }
  } else if (document.value().is_object()) {
    const JsonValue* records = document.value().member("records");
    if (records == nullptr || !records->is_array()) {
      return make_error(ErrorCode::kInvalidArgument,
                        "evidence document must be an array or an object with a records array");
    }
    for (const JsonValue& item : records->as_array()) {
      entries.push_back(&item);
    }
  } else {
    return make_error(ErrorCode::kInvalidArgument, "evidence document must be an array or object");
  }
  if (entries.size() > limits.max_snapshot_evidence) {
    return make_error(ErrorCode::kLimitExceeded, "evidence document contains too many records",
                      std::to_string(entries.size()));
  }

  std::vector<EvidenceRecord> records;
  records.reserve(entries.size());
  for (const JsonValue* entry : entries) {
    auto record = decode_ingest_json(entry->dump(0), limits);
    if (!record.ok()) {
      return record.error();
    }
    records.push_back(std::move(record.value()));
  }
  return records;
}

Result<std::vector<EvidenceRecord>> load_evidence_file(const std::string& path,
                                                       const Limits& limits) {
  auto text = read_text_file(path, limits.max_document_bytes);
  if (!text.ok()) {
    return text.error();
  }
  return load_evidence_document(text.value(), limits);
}

std::string describe_topology(const Topology& topology) {
  std::string out;
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "generation=%s nodes=%zu ports=%zu links=%zu queues=%zu buffers=%zu paths=%zu "
                "flows=%zu digest=%s\n",
                topology.generation().str().c_str(), topology.nodes().size(),
                topology.ports().size(), topology.links().size(), topology.queues().size(),
                topology.buffers().size(), topology.paths().size(), topology.flows().size(),
                to_hex(topology.digest()).c_str());
  out += buffer;
  for (const auto& entry : topology.links()) {
    std::snprintf(buffer, sizeof(buffer), "  link %s %s <-> %s capacity=%llu bps\n",
                  entry.second.id.str().c_str(), entry.second.endpoint_a.str().c_str(),
                  entry.second.endpoint_b.str().c_str(),
                  static_cast<unsigned long long>(entry.second.capacity_bps));
    out += buffer;
  }
  return out;
}

std::string describe_assessment(const CongestionAssessment& assessment) {
  std::string out;
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "subject=%s verdict=%s severity=%s confidence=%u mechanisms=%s\n",
                assessment.subject.str().c_str(), std::string(to_string(assessment.verdict)).c_str(),
                std::string(to_string(assessment.severity)).c_str(), assessment.confidence,
                describe_mechanisms(assessment.mechanisms).c_str());
  out += buffer;
  std::snprintf(buffer, sizeof(buffer),
                "  records=%zu fresh=%zu stale=%zu expired=%zu unknown=%zu sources=%zu\n",
                assessment.features.records_in_window, assessment.features.records_fresh,
                assessment.features.records_stale, assessment.features.records_expired,
                assessment.features.records_unknown_freshness, assessment.features.deciding_sources);
  out += buffer;
  std::snprintf(buffer, sizeof(buffer),
                "  utilization=%.4f present=%d pressure=%.4f present=%d demand_ratio=%.4f "
                "impairment=%d drop_rate=%.6g latency_factor=%.3f\n",
                assessment.features.utilization, assessment.features.utilization_present ? 1 : 0,
                assessment.features.pressure_ratio, assessment.features.pressure_present ? 1 : 0,
                assessment.features.demand_capacity_ratio,
                assessment.features.impairment_present ? 1 : 0, assessment.features.drop_rate,
                assessment.features.latency_factor);
  out += buffer;
  for (const Blocker& blocker : assessment.blockers) {
    out += "  blocker ";
    out += blocker.code;
    out += ": ";
    out += blocker.detail;
    out += "\n";
  }
  out += assessment.explanation.render();
  return out;
}

}  // namespace congestion::tools
