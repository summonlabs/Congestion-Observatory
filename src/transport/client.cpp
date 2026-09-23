// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/transport/client.hpp"

#include <utility>
#include <vector>

#include "congestion/core/json.hpp"
#include "congestion/transport/frame.hpp"
#include "congestion/transport/server.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

// One request/response exchange over an established connection. The client is strictly
// synchronous: it never has two frames in flight, which keeps the protocol and the tests
// deterministic.
Result<std::string> exchange(Socket& socket, FrameType request_type, const std::string& payload,
                             FrameType expected_type, const Limits& limits) {
  Frame request;
  request.type = request_type;
  request.payload = payload;
  auto encoded = encode_frame(request, limits);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const Status sent = socket.send_all(encoded.value().data(), encoded.value().size());
  if (!sent.ok()) {
    return sent.error();
  }

  FrameDecoder decoder(limits);
  std::vector<char> buffer(16 * 1024);
  while (true) {
    auto next = decoder.next();
    if (!next.ok()) {
      return next.error();
    }
    if (next.value() != nullptr) {
      const Frame& frame = *next.value();
      if (frame.type == FrameType::kError) {
        auto document = parse_json(frame.payload, limits);
        std::string message = "server reported an error";
        std::string detail = frame.payload;
        if (document.ok() && document.value().is_object()) {
          const JsonValue* value = document.value().member("message");
          if (value != nullptr) {
            message = value->as_string();
          }
          const JsonValue* detail_value = document.value().member("detail");
          if (detail_value != nullptr) {
            detail = detail_value->as_string();
          }
        }
        return make_error(ErrorCode::kProtocolError, message, detail);
      }
      if (frame.type != expected_type) {
        return make_error(ErrorCode::kProtocolError, "unexpected frame type",
                          std::string(to_string(frame.type)) + " != " +
                              std::string(to_string(expected_type)));
      }
      std::string response = frame.payload;
      decoder.consume();
      return response;
    }
    auto received = socket.recv_some(buffer.data(), buffer.size());
    if (!received.ok()) {
      return received.error();
    }
    if (received.value() == 0) {
      return make_error(ErrorCode::kIoError, "peer closed the connection before replying");
    }
    const Status fed = decoder.feed(buffer.data(), received.value());
    if (!fed.ok()) {
      return fed.error();
    }
  }
}

Result<std::string> greet(Socket& socket, const std::string& client_id, const Limits& limits) {
  auto response = exchange(socket, FrameType::kHello, encode_hello_json(client_id),
                           FrameType::kWelcome, limits);
  if (!response.ok()) {
    return response.error();
  }
  auto document = parse_json(response.value(), limits);
  if (!document.ok()) {
    return document.error();
  }
  std::string version = version_string();
  if (document.value().is_object()) {
    const JsonValue* value = document.value().member("version");
    if (value != nullptr && value->is_string()) {
      version = value->as_string();
    }
  }
  return version;
}

}  // namespace

Result<std::string> ping_server(const IngestClientConfig& config) {
  auto socket = Socket::connect_tcp(config.host, config.port, config.socket);
  if (!socket.ok()) {
    return socket.error();
  }
  auto version = greet(socket.value(), config.client_id, config.limits);
  if (!version.ok()) {
    return version.error();
  }
  auto pong = exchange(socket.value(), FrameType::kPing, std::string{}, FrameType::kPong,
                       config.limits);
  if (!pong.ok()) {
    return pong.error();
  }
  const auto bye = exchange(socket.value(), FrameType::kBye, std::string{}, FrameType::kBye,
                            config.limits);
  (void)bye;
  return version;
}

Result<IngestClientResult> push_records(const IngestClientConfig& config,
                                        const std::vector<EvidenceRecord>& records) {
  auto socket = Socket::connect_tcp(config.host, config.port, config.socket);
  if (!socket.ok()) {
    return socket.error();
  }
  auto version = greet(socket.value(), config.client_id, config.limits);
  if (!version.ok()) {
    return version.error();
  }

  IngestClientResult result;
  result.server_version = version.value();
  for (const EvidenceRecord& record : records) {
    auto response = exchange(socket.value(), FrameType::kIngest, encode_ingest_json(record),
                             FrameType::kIngestAck, config.limits);
    if (!response.ok()) {
      return response.error();
    }
    auto document = parse_json(response.value(), config.limits);
    if (!document.ok()) {
      return document.error();
    }
    IngestOutcome outcome;
    const JsonValue& root = document.value();
    if (const JsonValue* accepted = root.member("accepted")) {
      outcome.stored = accepted->as_bool(false);
    }
    if (const JsonValue* id = root.member("evidence_id")) {
      auto parsed = EvidenceId::parse(id->as_string());
      if (parsed.ok()) {
        outcome.id = parsed.value();
      }
    }
    if (const JsonValue* fence = root.member("fence")) {
      const std::string text = fence->as_string();
      for (int i = 0; i <= static_cast<int>(FenceDecision::kRejectedInconsistentBoot); ++i) {
        if (text == to_string(static_cast<FenceDecision>(i))) {
          outcome.fence = static_cast<FenceDecision>(i);
          break;
        }
      }
    }
    if (const JsonValue* detail = root.member("detail")) {
      outcome.detail = detail->as_string();
    }
    ++result.records_sent;
    if (outcome.stored) {
      ++result.records_accepted;
    } else {
      ++result.records_rejected;
    }
    result.outcomes.push_back(std::move(outcome));
  }
  const auto bye = exchange(socket.value(), FrameType::kBye, std::string{}, FrameType::kBye,
                            config.limits);
  (void)bye;
  return result;
}

}  // namespace congestion
