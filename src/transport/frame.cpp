// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/transport/frame.hpp"

#include <cstring>

#include "congestion/core/checked.hpp"
#include "congestion/core/crc64.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

std::uint32_t get_u32(const char* data) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  }
  return value;
}

std::uint64_t get_u64(const char* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  }
  return value;
}

}  // namespace

std::string_view to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::kHello: return "hello";
    case FrameType::kWelcome: return "welcome";
    case FrameType::kIngest: return "ingest";
    case FrameType::kIngestAck: return "ingest_ack";
    case FrameType::kStats: return "stats";
    case FrameType::kStatsReply: return "stats_reply";
    case FrameType::kPing: return "ping";
    case FrameType::kPong: return "pong";
    case FrameType::kBye: return "bye";
    case FrameType::kError: return "error";
  }
  return "error";
}

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits) {
  std::uint64_t body_length = 0;
  if (!checked_add<std::uint64_t>(frame.payload.size(), 8, body_length)) {
    return make_error(ErrorCode::kArithmeticOverflow, "frame body length overflow");
  }
  if (body_length > limits.max_frame_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "frame exceeds the configured bound",
                      std::to_string(body_length));
  }
  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderBytes + static_cast<std::size_t>(body_length));
  put_u32(out, kFrameMagic);
  put_u32(out, kProtocolVersion);
  put_u32(out, static_cast<std::uint32_t>(frame.type));
  put_u32(out, frame.flags);
  put_u64(out, body_length);
  const std::uint64_t crc =
      frame.payload.empty()
          ? crc64(std::string_view{})
          : crc64(std::string_view(frame.payload.data(), frame.payload.size()));
  for (const char c : frame.payload) {
    out.push_back(static_cast<std::uint8_t>(c));
  }
  put_u64(out, crc);
  return out;
}

Status FrameDecoder::feed(const char* data, std::size_t size) {
  std::size_t next_size = 0;
  if (!checked_add<std::size_t>(buffer_.size(), size, next_size)) {
    return Status(make_error(ErrorCode::kArithmeticOverflow, "frame buffer size overflow"));
  }
  const std::size_t bound = limits_.max_frame_bytes + kFrameHeaderBytes;
  if (next_size > bound) {
    return Status(make_error(ErrorCode::kLimitExceeded,
                             "frame buffer would exceed the configured bound",
                             std::to_string(next_size)));
  }
  buffer_.insert(buffer_.end(), data, data + size);
  return Status{};
}

Result<const Frame*> FrameDecoder::next() {
  if (have_current_) {
    return &current_;
  }
  if (buffer_.size() < kFrameHeaderBytes) {
    return static_cast<const Frame*>(nullptr);
  }
  const char* header = buffer_.data();
  if (get_u32(header) != kFrameMagic) {
    return make_error(ErrorCode::kProtocolError, "frame magic does not match");
  }
  const std::uint32_t version = get_u32(header + 4);
  if (version != kProtocolVersion) {
    return make_error(ErrorCode::kProtocolError, "frame protocol version differs",
                      std::to_string(version));
  }
  const std::uint64_t body_length = get_u64(header + 16);
  if (body_length < 8) {
    return make_error(ErrorCode::kProtocolError, "frame body is shorter than its checksum");
  }
  if (body_length > limits_.max_frame_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "declared frame length exceeds the configured bound",
                      std::to_string(body_length));
  }
  std::size_t total = 0;
  if (!checked_add<std::size_t>(kFrameHeaderBytes, static_cast<std::size_t>(body_length), total)) {
    return make_error(ErrorCode::kArithmeticOverflow, "frame length overflow");
  }
  if (buffer_.size() < total) {
    return static_cast<const Frame*>(nullptr);
  }
  current_.type = static_cast<FrameType>(get_u32(header + 8));
  current_.flags = get_u32(header + 12);
  const std::size_t payload_length = static_cast<std::size_t>(body_length) - 8;
  current_.payload.assign(header + kFrameHeaderBytes, payload_length);
  current_.crc64 = get_u64(header + kFrameHeaderBytes + payload_length);
  const std::uint64_t actual =
      payload_length == 0
          ? crc64(std::string_view{})
          : crc64(std::string_view(header + kFrameHeaderBytes, payload_length));
  if (actual != current_.crc64) {
    return make_error(ErrorCode::kProtocolError, "frame payload checksum mismatch");
  }
  cursor_ = total;
  have_current_ = true;
  ++frames_decoded_;
  return &current_;
}

void FrameDecoder::consume() {
  if (!have_current_) {
    return;
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
  cursor_ = 0;
  have_current_ = false;
  current_ = Frame{};
}

}  // namespace congestion
