// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "congestion/transport/socket.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cotest {

congestion::Timestamp at(std::int64_t seconds_offset) {
  return congestion::Timestamp::from_unix_seconds(kBaseSeconds + seconds_offset).value();
}

congestion::Limits test_limits() { return congestion::Limits{}; }

congestion::GenerationVector test_generation(std::uint64_t epoch, std::uint64_t generation,
                                             std::uint64_t revision) {
  congestion::GenerationVector value;
  value.epoch = congestion::Epoch(epoch);
  value.generation = congestion::Generation(generation);
  value.revision = congestion::Revision(revision);
  return value;
}

congestion::Topology make_test_topology(const congestion::GenerationVector& generation) {
  congestion::TopologyBuilder builder;
  builder.set_limits(test_limits());
  builder.set_generation(generation);
  builder.set_revision_label(congestion::Name::unchecked("rev-1"));

  const char* nodes[4] = {"host1", "host2", "leaf1", "leaf2"};
  for (const char* name : nodes) {
    congestion::Node node;
    node.id = congestion::NodeId::unchecked(name);
    node.kind = name[0] == 'h' ? congestion::NodeKind::kHost : congestion::NodeKind::kSwitch;
    (void)builder.add_node(node);
  }
  struct PortSpec { const char* id; const char* node; std::uint64_t speed; };
  const PortSpec ports[6] = {{"host1/1", "host1", 25000000000ULL},
                             {"host2/1", "host2", 25000000000ULL},
                             {"leaf1/1", "leaf1", 25000000000ULL},
                             {"leaf1/2", "leaf1", 100000000000ULL},
                             {"leaf2/1", "leaf2", 100000000000ULL},
                             {"leaf2/2", "leaf2", 25000000000ULL}};
  for (const PortSpec& spec : ports) {
    congestion::Port port;
    port.id = congestion::PortId::unchecked(spec.id);
    port.node = congestion::NodeId::unchecked(spec.node);
    port.speed_bps = spec.speed;
    (void)builder.add_port(port);
  }
  struct LinkSpec { const char* id; const char* a; const char* b; std::uint64_t capacity; };
  const LinkSpec links[3] = {{"host1-leaf1", "host1/1", "leaf1/1", 25000000000ULL},
                             {"leaf1-leaf2", "leaf1/2", "leaf2/1", 100000000000ULL},
                             {"leaf2-host2", "leaf2/2", "host2/1", 25000000000ULL}};
  for (const LinkSpec& spec : links) {
    congestion::Link link;
    link.id = congestion::LinkId::unchecked(spec.id);
    link.endpoint_a = congestion::PortId::unchecked(spec.a);
    link.endpoint_b = congestion::PortId::unchecked(spec.b);
    link.capacity_bps = spec.capacity;
    (void)builder.add_link(link);
  }
  struct QueueSpec { const char* id; const char* link; std::uint32_t index; };
  const QueueSpec queues[4] = {{"leaf1-leaf2/q0", "leaf1-leaf2", 0},
                               {"leaf1-leaf2/q1", "leaf1-leaf2", 1},
                               {"host1-leaf1/q0", "host1-leaf1", 0},
                               {"leaf2-host2/q0", "leaf2-host2", 0}};
  for (const QueueSpec& spec : queues) {
    congestion::Queue queue;
    queue.id = congestion::QueueId::unchecked(spec.id);
    queue.link = congestion::LinkId::unchecked(spec.link);
    queue.index = spec.index;
    (void)builder.add_queue(queue);
  }
  struct BufferSpec { const char* id; const char* port; std::uint64_t cells; };
  const BufferSpec buffers[2] = {{"leaf1/2/buf0", "leaf1/2", 4096},
                                 {"leaf2/1/buf0", "leaf2/1", 4096}};
  for (const BufferSpec& spec : buffers) {
    congestion::Buffer buffer;
    buffer.id = congestion::BufferId::unchecked(spec.id);
    buffer.port = congestion::PortId::unchecked(spec.port);
    buffer.pool = 0;
    buffer.cells = spec.cells;
    (void)builder.add_buffer(buffer);
  }
  congestion::Path path;
  path.id = congestion::PathId::unchecked("host1-host2");
  path.hops = {congestion::LinkId::unchecked("host1-leaf1"),
               congestion::LinkId::unchecked("leaf1-leaf2"),
               congestion::LinkId::unchecked("leaf2-host2")};
  (void)builder.add_path(path);
  congestion::Flow flow;
  flow.id = congestion::FlowId::unchecked("flow-a");
  flow.path = congestion::PathId::unchecked("host1-host2");
  flow.tenant = congestion::TenantId::unchecked("tenant-a");
  flow.traffic_class = congestion::ClassId::unchecked("class-1");
  (void)builder.add_flow(flow);
  auto topology = builder.build(test_limits());
  return topology.ok() ? topology.value() : congestion::Topology{};
}

congestion::EvidenceRecord Source::record(congestion::EvidenceKind kind,
                                          const congestion::EvidenceSubject& subject, double value,
                                          congestion::ObservationUnit unit,
                                          congestion::ValueSemantics semantics,
                                          congestion::Timestamp observed_at,
                                          congestion::Duration validity) {
  ++sequence;
  congestion::EvidenceRecord result;
  result.kind = kind;
  result.subject = subject;
  result.provenance.source = id;
  result.provenance.authority = authority;
  result.provenance.transport = transport;
  result.provenance.collector = collector;
  result.fence.source = id;
  result.fence.boot = boot;
  result.fence.incarnation = incarnation;
  result.fence.gen = gen;
  result.fence.sequence = sequence;
  result.observed_at = observed_at;
  result.received_at = observed_at;
  result.clock = clock;
  result.value.scalar = value;
  result.value.unit = unit;
  result.value.semantics = semantics;
  result.validity = validity;
  result.id = congestion::compute_evidence_id(result);
  return result;
}

congestion::EvidenceRecord Source::ratio(congestion::EvidenceKind kind,
                                         const congestion::EvidenceSubject& subject, double value,
                                         congestion::Timestamp observed_at) {
  return record(kind, subject, value, congestion::ObservationUnit::kRatio,
                congestion::ValueSemantics::kGauge, observed_at);
}

congestion::EvidenceRecord Source::counter(congestion::EvidenceKind kind,
                                           const congestion::EvidenceSubject& subject,
                                           double value,
                                           congestion::ValueSemantics semantics,
                                           congestion::Timestamp observed_at) {
  return record(kind, subject, value, congestion::ObservationUnit::kPackets, semantics,
                observed_at);
}

Source make_source(const std::string& name, const congestion::GenerationVector& generation) {
  Source source;
  source.id = congestion::SourceId::unchecked(name);
  source.gen = generation;
  return source;
}

congestion::EvidenceSubject link_subject(const char* name) {
  return congestion::EvidenceSubject(congestion::LinkId::unchecked(name));
}

congestion::EvidenceSubject queue_subject(const char* name) {
  return congestion::EvidenceSubject(congestion::QueueId::unchecked(name));
}

congestion::RuntimeConfig make_runtime_config(const congestion::Topology& topology,
                                              const congestion::Limits& limits) {
  congestion::RuntimeConfig config;
  config.limits = limits;
  // Test runtimes are synchronous unless a test asks for workers explicitly: deterministic
  // scheduling is the default, background work is opt in.
  config.limits.worker_threads = 0;
  config.topology = topology;
  config.start_workers = false;
  return config;
}

std::unique_ptr<congestion::Runtime> make_runtime(const congestion::Topology& topology,
                                                  const congestion::Limits& limits) {
  auto created = congestion::Runtime::create(make_runtime_config(topology, limits));
  if (!created.ok()) {
    return nullptr;
  }
  return std::move(created.value());
}

std::vector<const congestion::EvidenceRecord*> pointers_to(
    const std::vector<congestion::EvidenceRecord>& records) {
  std::vector<const congestion::EvidenceRecord*> pointers;
  pointers.reserve(records.size());
  for (const congestion::EvidenceRecord& record : records) {
    pointers.push_back(&record);
  }
  return pointers;
}

std::string unique_temp_dir(const std::string& tag) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t index = counter.fetch_add(1) + 1;
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  const std::filesystem::path directory =
      base / ("congestion-observatory-test-" + tag + "-" + std::to_string(index));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory.string();
}

std::string write_temp_file(const std::string& directory, const std::string& name,
                            const std::string& content) {
  const std::filesystem::path path = std::filesystem::path(directory) / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
  stream.close();
  return path.string();
}

ChildProcess::~ChildProcess() {
  if (running()) {
    terminate();
  }
#if defined(_WIN32)
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#endif
}

bool ChildProcess::start(const std::string& command_line) {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    return false;
  }
  std::string mutable_line = command_line;
  STARTUPINFOA startup;
  ZeroMemory(&startup, sizeof(startup));
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information;
  ZeroMemory(&information, sizeof(information));
  const BOOL created = CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, 0,
                                      nullptr, nullptr, &startup, &information);
  if (created == FALSE) {
    return false;
  }
  CloseHandle(information.hThread);
  handle_ = information.hProcess;
  pid_ = information.dwProcessId;
  return true;
#else
  (void)command_line;
  return false;
#endif
}

bool ChildProcess::running() const {
#if defined(_WIN32)
  if (handle_ == nullptr) {
    return false;
  }
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == FALSE) {
    return false;
  }
  return code == STILL_ACTIVE;
#else
  return false;
#endif
}

int ChildProcess::wait() {
#if defined(_WIN32)
  if (handle_ == nullptr) {
    return -1;
  }
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == FALSE) {
    return -1;
  }
  return static_cast<int>(code);
#else
  return -1;
#endif
}

bool ChildProcess::terminate() {
#if defined(_WIN32)
  if (handle_ == nullptr) {
    return true;
  }
  if (!running()) {
    return true;
  }
  if (TerminateProcess(static_cast<HANDLE>(handle_), 0) == FALSE) {
    return false;
  }
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  return true;
#else
  return true;
#endif
}

bool wait_for_port(const std::string& host, std::uint16_t port, std::uint32_t max_attempts) {
  congestion::SocketOptions options;
  options.io_deadline = congestion::Duration::from_millis(100);
  for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    auto socket = congestion::Socket::connect_tcp(host, port, options);
    if (socket.ok()) {
      socket.value().close();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string cli_path() { return std::string(CO_CLI_PATH); }

std::string quote(const std::string& text) { return std::string("\"") + text + "\""; }

int run_cli(const std::string& arguments) {
  ChildProcess process;
  if (!process.start(quote(cli_path()) + " " + arguments)) {
    return -1;
  }
  return process.wait();
}

void remove_tree(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

}  // namespace cotest
