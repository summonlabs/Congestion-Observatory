// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/transport/server.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "congestion/core/checked.hpp"
#include "congestion/core/json.hpp"
#include "congestion/transport/frame.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

std::string json_string(const JsonValue* value, const char* fallback) {
  if (value == nullptr || !value->is_string()) {
    return fallback;
  }
  return value->as_string();
}

Result<std::uint64_t> json_u64(const JsonValue* value, const char* what) {
  if (value == nullptr || (!value->is_int() && !value->is_double())) {
    return make_error(ErrorCode::kInvalidArgument, std::string("missing numeric field: ") + what);
  }
  const std::int64_t raw = value->as_int();
  if (raw < 0) {
    return make_error(ErrorCode::kOutOfRange, std::string("field must not be negative: ") + what);
  }
  return static_cast<std::uint64_t>(raw);
}

Result<double> json_number(const JsonValue* value, const char* what) {
  if (value == nullptr || (!value->is_int() && !value->is_double())) {
    return make_error(ErrorCode::kInvalidArgument, std::string("missing numeric field: ") + what);
  }
  return value->as_double();
}

}  // namespace

std::string encode_hello_json(const std::string& client_id) {
  JsonObject root;
  root.set("client", JsonValue(client_id));
  root.set("protocol", JsonValue(static_cast<std::int64_t>(kProtocolVersion)));
  root.set("version", JsonValue(version_string()));
  return JsonValue(std::move(root)).dump(0);
}

std::string encode_ingest_json(const EvidenceRecord& record) {
  JsonObject root;
  root.set("kind", JsonValue(std::string(to_string(record.kind))));
  root.set("subject", JsonValue(record.subject.str()));
  root.set("source", JsonValue(record.provenance.source.str()));
  root.set("authority", JsonValue(std::string(to_string(record.provenance.authority))));
  root.set("transport", JsonValue(record.provenance.transport));
  root.set("collector", JsonValue(record.provenance.collector));
  root.set("boot", JsonValue(static_cast<std::int64_t>(record.fence.boot.value())));
  root.set("incarnation", JsonValue(static_cast<std::int64_t>(record.fence.incarnation.value())));
  root.set("epoch", JsonValue(static_cast<std::int64_t>(record.fence.gen.epoch.value())));
  root.set("generation", JsonValue(static_cast<std::int64_t>(record.fence.gen.generation.value())));
  root.set("revision", JsonValue(static_cast<std::int64_t>(record.fence.gen.revision.value())));
  root.set("sequence", JsonValue(static_cast<std::int64_t>(record.fence.sequence.value())));
  root.set("observed_at", JsonValue(record.observed_at.to_iso8601()));
  root.set("received_at", JsonValue(record.received_at.to_iso8601()));
  root.set("clock", JsonValue(std::string(to_string(record.clock))));
  root.set("value", JsonValue(record.value.scalar));
  root.set("unit", JsonValue(std::string(to_string(record.value.unit))));
  root.set("semantics", JsonValue(std::string(to_string(record.value.semantics))));
  root.set("validity_ns", JsonValue(static_cast<std::int64_t>(record.validity.nanos())));
  root.set("completeness", JsonValue(std::string(to_string(record.completeness))));
  root.set("support", JsonValue(std::string(to_string(record.support))));
  root.set("note", JsonValue(record.note));
  JsonValue::Array labels;
  for (const Name& label : record.labels) {
    labels.push_back(JsonValue(label.str()));
  }
  root.set("labels", JsonValue(std::move(labels)));
  JsonObject metadata;
  for (const auto& entry : record.metadata) {
    metadata.set(entry.first, JsonValue(entry.second));
  }
  root.set("metadata", JsonValue(std::move(metadata)));
  return JsonValue(std::move(root)).dump(0);
}

Result<EvidenceRecord> decode_ingest_json(const std::string& payload, const Limits& limits) {
  auto document = parse_json(payload, limits);
  if (!document.ok()) {
    return document.error();
  }
  if (!document.value().is_object()) {
    return make_error(ErrorCode::kInvalidArgument, "ingest payload must be a JSON object");
  }
  const JsonValue& root = document.value();
  EvidenceRecord record;

  auto kind = evidence_kind_from_string(json_string(root.member("kind"), ""));
  if (!kind.ok()) {
    return kind.error();
  }
  record.kind = kind.value();

  auto subject = parse_subject(json_string(root.member("subject"), ""));
  if (!subject.ok()) {
    return subject.error();
  }
  record.subject = subject.value();

  auto source = SourceId::parse(json_string(root.member("source"), ""));
  if (!source.ok()) {
    return source.error();
  }
  record.provenance.source = source.value();
  record.fence.source = source.value();

  auto authority = authority_from_string(json_string(root.member("authority"), "unknown"));
  if (!authority.ok()) {
    return authority.error();
  }
  record.provenance.authority = authority.value();
  record.provenance.transport = json_string(root.member("transport"), "transport");
  record.provenance.collector = json_string(root.member("collector"), "collector");

  auto boot = json_u64(root.member("boot"), "boot");
  if (!boot.ok()) return boot.error();
  auto incarnation = json_u64(root.member("incarnation"), "incarnation");
  if (!incarnation.ok()) return incarnation.error();
  auto epoch = json_u64(root.member("epoch"), "epoch");
  if (!epoch.ok()) return epoch.error();
  auto generation = json_u64(root.member("generation"), "generation");
  if (!generation.ok()) return generation.error();
  auto revision = json_u64(root.member("revision"), "revision");
  if (!revision.ok()) return revision.error();
  auto sequence = json_u64(root.member("sequence"), "sequence");
  if (!sequence.ok()) return sequence.error();
  record.fence.boot = BootId(boot.value());
  record.fence.incarnation = Incarnation(incarnation.value());
  record.fence.gen.epoch = Epoch(epoch.value());
  record.fence.gen.generation = Generation(generation.value());
  record.fence.gen.revision = Revision(revision.value());
  record.fence.sequence = Sequence(sequence.value());

  auto observed = Timestamp::from_iso8601(json_string(root.member("observed_at"), ""));
  if (!observed.ok()) {
    return observed.error();
  }
  record.observed_at = observed.value();
  auto received = Timestamp::from_iso8601(json_string(root.member("received_at"), ""));
  if (!received.ok()) {
    return received.error();
  }
  record.received_at = received.value();

  auto clock = clock_domain_from_string(json_string(root.member("clock"), "unknown"));
  if (!clock.ok()) {
    return clock.error();
  }
  record.clock = clock.value();

  auto value = json_number(root.member("value"), "value");
  if (!value.ok()) return value.error();
  record.value.scalar = value.value();
  auto unit = observation_unit_from_string(json_string(root.member("unit"), "unknown"));
  if (!unit.ok()) return unit.error();
  record.value.unit = unit.value();
  auto semantics = value_semantics_from_string(json_string(root.member("semantics"), "unknown"));
  if (!semantics.ok()) return semantics.error();
  record.value.semantics = semantics.value();

  if (const JsonValue* validity = root.member("validity_ns")) {
    const std::int64_t nanos = validity->as_int(5000000000LL);
    if (nanos < 0) {
      return make_error(ErrorCode::kOutOfRange, "validity_ns must not be negative");
    }
    record.validity = Duration(nanos);
  }
  auto completeness = completeness_from_string(json_string(root.member("completeness"), "complete"));
  if (!completeness.ok()) return completeness.error();
  record.completeness = completeness.value();
  auto support = support_from_string(json_string(root.member("support"), "supported"));
  if (!support.ok()) return support.error();
  record.support = support.value();
  record.note = json_string(root.member("note"), "");

  if (const JsonValue* labels = root.member("labels")) {
    if (labels->is_array()) {
      for (const JsonValue& item : labels->as_array()) {
        if (!item.is_string()) {
          return make_error(ErrorCode::kInvalidArgument, "labels must be strings");
        }
        auto label = Name::parse(item.as_string());
        if (!label.ok()) {
          return label.error();
        }
        if (record.labels.size() >= limits.max_labels) {
          return make_error(ErrorCode::kLimitExceeded, "label count exceeds the configured limit");
        }
        record.labels.push_back(label.value());
      }
    }
  }
  if (const JsonValue* metadata = root.member("metadata")) {
    if (metadata->is_object()) {
      for (const auto& entry : metadata->as_object().members()) {
        if (record.metadata.size() >= limits.max_metadata_entries) {
          return make_error(ErrorCode::kLimitExceeded,
                            "metadata entry count exceeds the configured limit");
        }
        if (entry.first.size() > limits.max_metadata_key_bytes ||
            entry.second.as_string().size() > limits.max_metadata_value_bytes) {
          return make_error(ErrorCode::kLimitExceeded, "metadata entry exceeds the configured bound");
        }
        record.metadata.emplace_back(entry.first, entry.second.as_string());
      }
    }
  }
  record.id = compute_evidence_id(record);
  const Status valid = validate_evidence(record, limits);
  if (!valid.ok()) {
    return valid.error();
  }
  return record;
}

std::string encode_ingest_ack_json(const IngestOutcome& outcome) {
  JsonObject root;
  root.set("accepted", JsonValue(outcome.stored));
  root.set("evidence_id", JsonValue(outcome.id.str()));
  root.set("fence", JsonValue(std::string(to_string(outcome.fence))));
  root.set("freshness", JsonValue(std::string(to_string(outcome.freshness))));
  root.set("detail", JsonValue(outcome.detail));
  root.set("evicted", JsonValue(static_cast<std::int64_t>(outcome.evicted)));
  root.set("liveness_reset", JsonValue(outcome.liveness_reset));
  root.set("sequence_gap", JsonValue(outcome.sequence_gap));
  return JsonValue(std::move(root)).dump(0);
}

std::string encode_error_json(ErrorCode code, const std::string& message,
                              const std::string& detail) {
  JsonObject root;
  root.set("error", JsonValue(std::string(to_string(code))));
  root.set("message", JsonValue(message));
  root.set("detail", JsonValue(detail));
  return JsonValue(std::move(root)).dump(0);
}

std::string encode_stats_json(const ServerStats& stats) {
  JsonObject root;
  root.set("connections_accepted", JsonValue(static_cast<std::int64_t>(stats.connections_accepted)));
  root.set("connections_rejected", JsonValue(static_cast<std::int64_t>(stats.connections_rejected)));
  root.set("frames_received", JsonValue(static_cast<std::int64_t>(stats.frames_received)));
  root.set("frames_sent", JsonValue(static_cast<std::int64_t>(stats.frames_sent)));
  root.set("records_accepted", JsonValue(static_cast<std::int64_t>(stats.records_accepted)));
  root.set("records_rejected", JsonValue(static_cast<std::int64_t>(stats.records_rejected)));
  root.set("protocol_errors", JsonValue(static_cast<std::int64_t>(stats.protocol_errors)));
  root.set("version", JsonValue(version_string()));
  return JsonValue(std::move(root)).dump(0);
}

IngestServer::IngestServer(Runtime& runtime, ServerConfig config)
    : runtime_(runtime), config_(std::move(config)) {}

IngestServer::~IngestServer() = default;

Result<std::uint16_t> IngestServer::start() {
  auto listener = TcpListener::bind_loopback(config_.port, config_.socket);
  if (!listener.ok()) {
    return listener.error();
  }
  listener_ = std::move(listener.value());
  port_ = listener_.port();
  return port_;
}

void IngestServer::stop() {
  stop_requested_.store(true, std::memory_order_release);
  // Closing the listener releases a thread that is blocked in accept(). Workers are still joined
  // by run(), so no connection is abandoned.
  listener_.close();
}

ServerStats IngestServer::stats() const {
  const std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

Status IngestServer::run(const CancellationToken& token) {
  if (!listener_.valid()) {
    return Status(make_error(ErrorCode::kPreconditionFailed, "server has not been started"));
  }
  // Connections are served on bounded worker threads: a peer that connects and then stays silent
  // cannot stall every other collector. Threads are joined before run() returns.
  struct ConnectionWorker {
    std::thread thread{};
    std::shared_ptr<std::atomic<bool>> finished{};
  };
  std::vector<ConnectionWorker> workers;

  const auto reap_finished = [&workers]() {
    for (auto it = workers.begin(); it != workers.end();) {
      if (it->finished->load(std::memory_order_acquire)) {
        if (it->thread.joinable()) {
          it->thread.join();
        }
        it = workers.erase(it);
      } else {
        ++it;
      }
    }
  };

  while (!stop_requested_.load(std::memory_order_acquire) && !token.cancelled()) {
    auto connection = listener_.accept();
    if (!connection.ok()) {
      if (stop_requested_.load(std::memory_order_acquire) || token.cancelled()) {
        break;
      }
      for (ConnectionWorker& worker : workers) {
        if (worker.thread.joinable()) {
          worker.thread.join();
        }
      }
      return connection.error();
    }
    reap_finished();
    if (workers.size() >= config_.max_connections) {
      // Refuse politely and keep serving: the limit is a resource bound, not a failure.
      {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.connections_rejected;
      }
      Socket refused = std::move(connection.value());
      Frame frame;
      frame.type = FrameType::kError;
      frame.payload = encode_error_json(ErrorCode::kQueueFull, "connection limit reached",
                                        std::to_string(config_.max_connections));
      auto encoded = encode_frame(frame, runtime_.limits());
      if (encoded.ok()) {
        (void)refused.send_all(encoded.value().data(), encoded.value().size());
      }
      refused.close();
      continue;
    }

    ConnectionWorker worker;
    worker.finished = std::make_shared<std::atomic<bool>>(false);
    Socket socket = std::move(connection.value());
    const std::shared_ptr<std::atomic<bool>> finished = worker.finished;
    worker.thread = std::thread([this, socket = std::move(socket), token, finished]() mutable {
      (void)serve_connection(std::move(socket), token);
      finished->store(true, std::memory_order_release);
    });
    workers.push_back(std::move(worker));
  }

  for (ConnectionWorker& worker : workers) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  return Status{};
}

Status IngestServer::serve_connection(Socket socket, const CancellationToken& token) {
  {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.connections_accepted;
  }
  FrameDecoder decoder(runtime_.limits());
  bool greeted = false;
  std::vector<char> buffer(16 * 1024);

  const auto send_frame = [&](FrameType type, const std::string& payload) -> Status {
    Frame frame;
    frame.type = type;
    frame.payload = payload;
    auto encoded = encode_frame(frame, runtime_.limits());
    if (!encoded.ok()) {
      return encoded.error();
    }
    const Status sent = socket.send_all(encoded.value().data(), encoded.value().size());
    if (sent.ok()) {
      const std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.frames_sent;
    }
    return sent;
  };

  const auto fail = [&](ErrorCode code, const std::string& message,
                        const std::string& detail) -> Status {
    {
      const std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.protocol_errors;
    }
    const Status sent = send_frame(FrameType::kError, encode_error_json(code, message, detail));
    if (!sent.ok()) {
      return sent;
    }
    return Status(make_error(code, message, detail));
  };

  while (!token.cancelled() && !stop_requested_.load(std::memory_order_acquire)) {
    auto received = socket.recv_some(buffer.data(), buffer.size());
    if (!received.ok()) {
      return received.error();
    }
    if (received.value() == 0) {
      return Status{};  // orderly shutdown by the peer
    }
    const Status fed = decoder.feed(buffer.data(), received.value());
    if (!fed.ok()) {
      return fail(fed.code(), "frame rejected", fed.error().describe());
    }
    while (true) {
      auto next = decoder.next();
      if (!next.ok()) {
        return fail(next.error().code(), "frame decode failed", next.error().describe());
      }
      if (next.value() == nullptr) {
        break;
      }
      const Frame& frame = *next.value();
      {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.frames_received;
      }
      switch (frame.type) {
        case FrameType::kHello: {
          greeted = true;
          auto document = parse_json(frame.payload, runtime_.limits());
          std::string client_id = "unknown";
          if (document.ok() && document.value().is_object()) {
            client_id = json_string(document.value().member("client"), "unknown");
          }
          const Status sent = send_frame(FrameType::kWelcome, encode_hello_json(client_id));
          if (!sent.ok()) {
            return sent;
          }
          break;
        }
        case FrameType::kIngest: {
          if (config_.require_hello && !greeted) {
            decoder.consume();
            return fail(ErrorCode::kProtocolError, "hello frame required",
                        "the first frame must be a hello");
          }
          auto record = decode_ingest_json(frame.payload, runtime_.limits());
          if (!record.ok()) {
            const Status sent = send_frame(
                FrameType::kIngestAck,
                encode_ingest_ack_json(IngestOutcome{}));
            if (!sent.ok()) {
              return sent;
            }
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.records_rejected;
            break;
          }
          const Timestamp now = config_.use_override_time ? config_.now_source_override
                                                          : now_wall_clock();
          auto outcome = runtime_.ingest(record.value(), now);
          if (!outcome.ok()) {
            IngestOutcome rejected;
            rejected.stored = false;
            rejected.id = record.value().id;
            rejected.detail = outcome.error().describe();
            const Status sent = send_frame(FrameType::kIngestAck, encode_ingest_ack_json(rejected));
            if (!sent.ok()) {
              return sent;
            }
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.records_rejected;
            break;
          }
          const Status sent =
              send_frame(FrameType::kIngestAck, encode_ingest_ack_json(outcome.value()));
          if (!sent.ok()) {
            return sent;
          }
          std::lock_guard<std::mutex> lock(stats_mutex_);
          if (outcome.value().stored) {
            ++stats_.records_accepted;
          } else {
            ++stats_.records_rejected;
          }
          break;
        }
        case FrameType::kPing: {
          const Status sent = send_frame(FrameType::kPong, version_string());
          if (!sent.ok()) {
            return sent;
          }
          break;
        }
        case FrameType::kStats: {
          const Status sent = send_frame(FrameType::kStatsReply, encode_stats_json(stats()));
          if (!sent.ok()) {
            return sent;
          }
          break;
        }
        case FrameType::kBye: {
          const Status sent = send_frame(FrameType::kBye, std::string{});
          decoder.consume();
          return sent;
        }
        default: {
          decoder.consume();
          return fail(ErrorCode::kProtocolError, "unsupported frame type",
                      std::string(to_string(frame.type)));
        }
      }
      decoder.consume();
    }
  }
  const Status sent = send_frame(FrameType::kBye, std::string{});
  return sent;
}

}  // namespace congestion
