// Congestion Observatory - evidence ingestion server.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TRANSPORT_SERVER_HPP
#define CONGESTION_TRANSPORT_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "congestion/core/cancel.hpp"
#include "congestion/runtime/runtime.hpp"
#include "congestion/transport/socket.hpp"

namespace congestion {

struct ServerConfig {
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{0};
  SocketOptions socket{};
  std::size_t max_connections{8};
  bool require_hello{true};
  Timestamp now_source_override{};  // zero: use each connection's receive time
  bool use_override_time{false};
};

struct ServerStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_rejected{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_sent{0};
  std::uint64_t records_accepted{0};
  std::uint64_t records_rejected{0};
  std::uint64_t protocol_errors{0};
};

// Serves the frame protocol on a loopback TCP socket. Each connection runs on its own bounded
// worker; connections are limited by ServerConfig::max_connections. Shutdown is cooperative and
// joins every worker: run() returns only after the last connection has ended.
class IngestServer {
 public:
  IngestServer(Runtime& runtime, ServerConfig config);
  ~IngestServer();

  // Binds and starts listening. Returns the bound port.
  [[nodiscard]] Result<std::uint16_t> start();

  // Accepts and serves connections until stop() is called or the listener fails.
  [[nodiscard]] Status run(const CancellationToken& token);

  void stop();
  [[nodiscard]] ServerStats stats() const;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  [[nodiscard]] Status serve_connection(Socket socket, const CancellationToken& token);

  Runtime& runtime_;
  ServerConfig config_{};
  TcpListener listener_{};
  std::uint16_t port_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> active_connections_{0};
  mutable std::mutex stats_mutex_;
  ServerStats stats_{};
};

// JSON helpers shared by client and server.
[[nodiscard]] std::string encode_hello_json(const std::string& client_id);
[[nodiscard]] std::string encode_ingest_json(const EvidenceRecord& record);
[[nodiscard]] Result<EvidenceRecord> decode_ingest_json(const std::string& payload,
                                                        const Limits& limits);
[[nodiscard]] std::string encode_ingest_ack_json(const IngestOutcome& outcome);
[[nodiscard]] std::string encode_error_json(ErrorCode code, const std::string& message,
                                            const std::string& detail);
[[nodiscard]] std::string encode_stats_json(const ServerStats& stats);

}  // namespace congestion

#endif  // CONGESTION_TRANSPORT_SERVER_HPP
