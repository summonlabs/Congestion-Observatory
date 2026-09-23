// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/model/identities.hpp"

namespace congestion {
namespace {

constexpr std::string_view kKindNames[] = {
    "none",   "node",   "port", "link",   "queue",    "buffer",
    "path",   "flow",   "tenant", "class", "source", "topology"};

std::string render(SubjectKind kind, const std::string& name) {
  std::string out;
  out.reserve(name.size() + 12);
  out += to_string(kind);
  out.push_back(':');
  out += name;
  return out;
}

template <class Id>
Result<EvidenceSubject> parse_subject_id(std::string_view name_text, const char* kind_label) {
  auto parsed = Id::parse(name_text);
  if (!parsed.ok()) {
    return make_error(ErrorCode::kInvalidArgument,
                      std::string("invalid ") + kind_label + " identity", std::string(name_text));
  }
  return EvidenceSubject(parsed.value());
}

}  // namespace

std::string_view to_string(SubjectKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= sizeof(kKindNames) / sizeof(kKindNames[0])) {
    return "none";
  }
  return kKindNames[index];
}

SubjectKind EvidenceSubject::kind() const noexcept {
  switch (storage_.index()) {
    case 1: return SubjectKind::kNode;
    case 2: return SubjectKind::kPort;
    case 3: return SubjectKind::kLink;
    case 4: return SubjectKind::kQueue;
    case 5: return SubjectKind::kBuffer;
    case 6: return SubjectKind::kPath;
    case 7: return SubjectKind::kFlow;
    case 8: return SubjectKind::kTenant;
    case 9: return SubjectKind::kClass;
    case 10: return SubjectKind::kSource;
    case 11: return SubjectKind::kTopology;
    default: return SubjectKind::kNone;
  }
}

std::string EvidenceSubject::str() const {
  switch (storage_.index()) {
    case 1: return render(SubjectKind::kNode, std::get<NodeId>(storage_).str());
    case 2: return render(SubjectKind::kPort, std::get<PortId>(storage_).str());
    case 3: return render(SubjectKind::kLink, std::get<LinkId>(storage_).str());
    case 4: return render(SubjectKind::kQueue, std::get<QueueId>(storage_).str());
    case 5: return render(SubjectKind::kBuffer, std::get<BufferId>(storage_).str());
    case 6: return render(SubjectKind::kPath, std::get<PathId>(storage_).str());
    case 7: return render(SubjectKind::kFlow, std::get<FlowId>(storage_).str());
    case 8: return render(SubjectKind::kTenant, std::get<TenantId>(storage_).str());
    case 9: return render(SubjectKind::kClass, std::get<ClassId>(storage_).str());
    case 10: return render(SubjectKind::kSource, std::get<SourceId>(storage_).str());
    case 11: return render(SubjectKind::kTopology, std::get<TopologyId>(storage_).str());
    default: return "none:";
  }
}

Result<EvidenceSubject> parse_subject(std::string_view text) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
    return make_error(ErrorCode::kInvalidArgument, "subject must be written as kind:name",
                      std::string(text));
  }
  const std::string_view kind_text = text.substr(0, separator);
  const std::string_view name_text = text.substr(separator + 1);

  if (kind_text == "node") return parse_subject_id<NodeId>(name_text, "node");
  if (kind_text == "port") return parse_subject_id<PortId>(name_text, "port");
  if (kind_text == "link") return parse_subject_id<LinkId>(name_text, "link");
  if (kind_text == "queue") return parse_subject_id<QueueId>(name_text, "queue");
  if (kind_text == "buffer") return parse_subject_id<BufferId>(name_text, "buffer");
  if (kind_text == "path") return parse_subject_id<PathId>(name_text, "path");
  if (kind_text == "flow") return parse_subject_id<FlowId>(name_text, "flow");
  if (kind_text == "tenant") return parse_subject_id<TenantId>(name_text, "tenant");
  if (kind_text == "class") return parse_subject_id<ClassId>(name_text, "class");
  if (kind_text == "source") return parse_subject_id<SourceId>(name_text, "source");
  if (kind_text == "topology") return parse_subject_id<TopologyId>(name_text, "topology");
  return make_error(ErrorCode::kUnsupported, "unknown subject kind", std::string(kind_text));
}

}  // namespace congestion
