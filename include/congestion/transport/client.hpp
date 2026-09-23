// Congestion Observatory - ingestion client used by tools and multi-process tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TRANSPORT_CLIENT_HPP
#define CONGESTION_TRANSPORT_CLIENT_HPP

#include <string>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/evidence/store.hpp"
#include "congestion/transport/socket.hpp"

namespace congestion {

struct IngestClientConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string client_id{"congestion-observatory-client"};
  SocketOptions socket{};
  Limits limits{};
};

struct IngestClientResult {
  std::uint64_t records_sent{0};
  std::uint64_t records_accepted{0};
  std::uint64_t records_rejected{0};
  std::vector<IngestOutcome> outcomes{};
  std::string server_version{};
};

// Pushes records to a remote runtime over the frame protocol. Used by the CLI and by the
// independent-process transport tests, where the client runs in a separate OS process.
[[nodiscard]] Result<IngestClientResult> push_records(const IngestClientConfig& config,
                                                      const std::vector<EvidenceRecord>& records);

// Round-trip used as a liveness probe.
[[nodiscard]] Result<std::string> ping_server(const IngestClientConfig& config);

}  // namespace congestion

#endif  // CONGESTION_TRANSPORT_CLIENT_HPP
