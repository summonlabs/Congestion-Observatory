// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace congestion;

namespace {

const char* kTransportTopologyJson = R"({
  "generation": {"epoch": 1, "generation": 1, "revision": 1},
  "revision_label": "transport-1",
  "nodes": [
    {"id": "leaf1", "kind": "switch"},
    {"id": "leaf2", "kind": "switch"}
  ],
  "ports": [
    {"id": "leaf1/1", "node": "leaf1", "speed_bps": 100000000000},
    {"id": "leaf2/1", "node": "leaf2", "speed_bps": 100000000000}
  ],
  "links": [
    {"id": "leaf1-leaf2", "endpoint_a": "leaf1/1", "endpoint_b": "leaf2/1",
     "capacity_bps": 100000000000}
  ]
})";

std::string evidence_document(const std::vector<EvidenceRecord>& records) {
  std::string document = "[";
  bool first = true;
  for (const EvidenceRecord& record : records) {
    if (!first) {
      document += ",";
    }
    first = false;
    document += encode_ingest_json(record);
  }
  document += "]";
  return document;
}

}  // namespace

CO_TEST(transport, frames_round_trip_and_are_bounded) {
  Limits limits = cotest::test_limits();
  limits.max_frame_bytes = 256;
  Frame frame;
  frame.type = FrameType::kIngest;
  frame.payload = "{\"a\":1}";
  auto encoded = encode_frame(frame, limits);
  CO_EXPECT_OK(encoded);

  FrameDecoder decoder(limits);
  // Feeding one byte at a time must produce the frame exactly once.
  const Frame* decoded = nullptr;
  for (const std::uint8_t byte : encoded.value()) {
    CO_EXPECT(decoder.feed(reinterpret_cast<const char*>(&byte), 1).ok());
    auto next = decoder.next();
    CO_EXPECT_OK(next);
    if (next.ok() && next.value() != nullptr) {
      decoded = next.value();
    }
  }
  CO_EXPECT(decoded != nullptr);
  if (decoded != nullptr) {
    CO_EXPECT_EQ(decoded->type, FrameType::kIngest);
    CO_EXPECT_EQ(decoded->payload, std::string("{\"a\":1}"));
  }
  CO_EXPECT_EQ(decoder.frames_decoded(), static_cast<std::size_t>(1));

  Frame oversized;
  oversized.type = FrameType::kIngest;
  oversized.payload.assign(limits.max_frame_bytes + 1, 'x');
  CO_EXPECT_ERR(encode_frame(oversized, limits), ErrorCode::kLimitExceeded);

  // A corrupted payload is refused rather than delivered.
  std::vector<std::uint8_t> corrupted = encoded.value();
  corrupted[corrupted.size() - 2] ^= 0x20;
  FrameDecoder corrupt_decoder(limits);
  CO_EXPECT(corrupt_decoder.feed(reinterpret_cast<const char*>(corrupted.data()), corrupted.size()).ok());
  CO_EXPECT_ERR(corrupt_decoder.next(), ErrorCode::kProtocolError);

  // A lying length header is refused before any allocation.
  std::vector<std::uint8_t> lying = encoded.value();
  for (int i = 0; i < 8; ++i) {
    lying[16 + i] = 0xFF;
  }
  FrameDecoder lying_decoder(limits);
  CO_EXPECT(lying_decoder.feed(reinterpret_cast<const char*>(lying.data()), lying.size()).ok());
  CO_EXPECT_ERR(lying_decoder.next(), ErrorCode::kLimitExceeded);

  // Bad magic.
  std::vector<std::uint8_t> magic = encoded.value();
  magic[0] = 0;
  FrameDecoder magic_decoder(limits);
  CO_EXPECT(magic_decoder.feed(reinterpret_cast<const char*>(magic.data()), magic.size()).ok());
  CO_EXPECT_ERR(magic_decoder.next(), ErrorCode::kProtocolError);
}

CO_TEST(transport, in_process_server_and_client_exchange_records) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  ServerConfig config;
  config.socket.io_deadline = Duration::from_seconds(10);
  IngestServer server(*runtime, config);
  auto port = server.start();
  CO_EXPECT_OK(port);
  if (!port.ok()) {
    return;
  }

  CancellationSource cancellation;
  std::thread server_thread([&]() { (void)server.run(cancellation.token()); });

  cotest::Source source = cotest::make_source("remote-collector", topology.generation());
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.02,
                                 cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.99,
                                 cotest::at(0)));

  IngestClientConfig client_config;
  client_config.port = port.value();
  client_config.socket.io_deadline = Duration::from_seconds(10);
  auto pushed = push_records(client_config, records);
  CO_EXPECT_OK(pushed);
  if (pushed.ok()) {
    CO_EXPECT_EQ(pushed.value().records_sent, static_cast<std::uint64_t>(2));
    CO_EXPECT_EQ(pushed.value().records_accepted, static_cast<std::uint64_t>(2));
    CO_EXPECT_EQ(pushed.value().records_rejected, static_cast<std::uint64_t>(0));
    CO_EXPECT(!pushed.value().server_version.empty());
  }
  auto version = ping_server(client_config);
  CO_EXPECT_OK(version);

  const ServerStats stats = server.stats();
  CO_EXPECT(stats.connections_accepted >= 2);
  CO_EXPECT(stats.records_accepted >= 2);
  CO_EXPECT_EQ(stats.protocol_errors, static_cast<std::uint64_t>(0));

  server.stop();
  cancellation.cancel("test complete");
  server_thread.join();

  CO_EXPECT_EQ(runtime->metrics().ingested, static_cast<std::uint64_t>(2));
  CO_EXPECT(!runtime->evidence().query(EvidenceQuery{}).empty());
}

CO_TEST(transport, server_reports_rejected_records_without_closing) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  ServerConfig config;
  config.socket.io_deadline = Duration::from_seconds(10);
  IngestServer server(*runtime, config);
  auto port = server.start();
  CO_EXPECT_OK(port);
  if (!port.ok()) {
    return;
  }
  CancellationSource cancellation;
  std::thread server_thread([&]() { (void)server.run(cancellation.token()); });

  cotest::Source source = cotest::make_source("remote-collector", topology.generation());
  EvidenceRecord valid = source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.02,
                                      cotest::at(0));
  EvidenceRecord unknown_subject = source.ratio(EvidenceKind::kLinkUtilization,
                                                cotest::link_subject("not-in-topology"), 0.5,
                                                cotest::at(0));

  IngestClientConfig client_config;
  client_config.port = port.value();
  client_config.socket.io_deadline = Duration::from_seconds(10);
  auto pushed = push_records(client_config, {valid, unknown_subject});
  CO_EXPECT_OK(pushed);
  if (pushed.ok()) {
    CO_EXPECT_EQ(pushed.value().records_accepted, static_cast<std::uint64_t>(1));
    CO_EXPECT_EQ(pushed.value().records_rejected, static_cast<std::uint64_t>(1));
  }

  server.stop();
  cancellation.cancel("test complete");
  server_thread.join();
  CO_EXPECT_EQ(runtime->metrics().ingest_rejected_unknown_subject, static_cast<std::uint64_t>(1));
}

CO_TEST(transport, independent_process_client_pushes_into_this_process) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  auto runtime = cotest::make_runtime(topology, cotest::test_limits());
  CO_EXPECT(runtime != nullptr);
  if (runtime == nullptr) {
    return;
  }
  ServerConfig config;
  config.socket.io_deadline = Duration::from_seconds(30);
  IngestServer server(*runtime, config);
  auto port = server.start();
  CO_EXPECT_OK(port);
  if (!port.ok()) {
    return;
  }
  CancellationSource cancellation;
  std::thread server_thread([&]() { (void)server.run(cancellation.token()); });

  const std::string directory = cotest::unique_temp_dir("transport-client");
  const std::string topology_path =
      cotest::write_temp_file(directory, "topology.json", kTransportTopologyJson);
  cotest::Source source = cotest::make_source("process-collector", topology.generation());
  std::vector<EvidenceRecord> records;
  for (int i = 0; i < 5; ++i) {
    records.push_back(source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.01,
                                   cotest::at(i)));
  }
  const std::string evidence_path =
      cotest::write_temp_file(directory, "evidence.json", evidence_document(records));

  const int exit_code = cotest::run_cli(
      "push --endpoint 127.0.0.1:" + std::to_string(port.value()) + " --file " +
      cotest::quote(evidence_path) + " --topology " + cotest::quote(topology_path) +
      " --io-deadline 30s");
  CO_EXPECT_EQ(exit_code, 0);

  server.stop();
  cancellation.cancel("test complete");
  server_thread.join();

  CO_EXPECT_EQ(runtime->metrics().ingested, static_cast<std::uint64_t>(5));
  CO_EXPECT_EQ(runtime->metrics().live_sources, static_cast<std::size_t>(1));
  cotest::remove_tree(directory);
}

CO_TEST(transport, this_process_pushes_into_an_independent_server_process) {
  const std::string directory = cotest::unique_temp_dir("transport-server");
  const std::string topology_path =
      cotest::write_temp_file(directory, "topology.json", kTransportTopologyJson);
  const std::string state_path = directory + "/state";

  const std::uint16_t port = 45913;
  // The child is started directly, not through cmd.exe, so the executable is quoted but the
  // command line is not wrapped.
  const std::string command = cotest::quote(cotest::cli_path()) + " serve --port " +
                              std::to_string(port) + " --topology " +
                              cotest::quote(topology_path) + " --state " +
                              cotest::quote(state_path) + " --io-deadline 30s";
  cotest::ChildProcess server;
  CO_REQUIRE(server.start(command));

  // Wait for the listener, but stop waiting as soon as the child is gone: a failed start up must
  // fail the test quickly instead of waiting out the budget.
  bool ready = false;
  for (int attempt = 0; attempt < 300 && !ready; ++attempt) {
    if (!server.running()) {
      break;
    }
    ready = cotest::wait_for_port("127.0.0.1", port, 1);
  }
  CO_REQUIRE(ready);

  IngestClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = port;
  client_config.socket.io_deadline = Duration::from_seconds(30);
  auto version = ping_server(client_config);
  CO_EXPECT_OK(version);

  cotest::Source source = cotest::make_source("client-process", cotest::test_generation());
  std::vector<EvidenceRecord> records;
  for (int i = 0; i < 3; ++i) {
    records.push_back(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.9,
                                   cotest::at(i)));
  }
  auto pushed = push_records(client_config, records);
  CO_EXPECT_OK(pushed);
  if (pushed.ok()) {
    CO_EXPECT_EQ(pushed.value().records_accepted, static_cast<std::uint64_t>(3));
  }

  CO_EXPECT(server.terminate());
  CO_EXPECT(!server.running());
  cotest::remove_tree(directory);
}

CO_TEST(transport, client_reports_a_refused_connection) {
  IngestClientConfig config;
  config.host = "127.0.0.1";
  config.port = 1;  // nothing listens here
  config.socket.io_deadline = Duration::from_millis(250);
  auto pinged = ping_server(config);
  CO_EXPECT(!pinged.ok());
  CO_EXPECT_ERR(push_records(config, {}), ErrorCode::kIoError);
}
