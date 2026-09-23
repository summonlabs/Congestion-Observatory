// Congestion Observatory - bounded frame protocol for independent-process ingestion.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TRANSPORT_FRAME_HPP
#define CONGESTION_TRANSPORT_FRAME_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/core/hash.hpp"

namespace congestion {

// Wire frame:  magic u32 ("COF1") | version u32 | type u32 | flags u32 | length u64 | payload
// The header is fixed width and little endian; length is validated against Limits::max_frame_bytes
// with checked arithmetic before a single payload byte is allocated.
enum class FrameType : std::uint32_t {
  kHello = 1,
  kWelcome = 2,
  kIngest = 3,
  kIngestAck = 4,
  kStats = 5,
  kStatsReply = 6,
  kPing = 7,
  kPong = 8,
  kBye = 9,
  kError = 10,
};

[[nodiscard]] std::string_view to_string(FrameType type) noexcept;

inline constexpr std::uint32_t kFrameMagic = 0x31464F43u;  // "COF1" little endian
inline constexpr std::size_t kFrameHeaderBytes = 4 + 4 + 4 + 4 + 8;

struct Frame {
  FrameType type{FrameType::kError};
  std::uint32_t flags{0};
  std::string payload{};
  std::uint64_t crc64{0};  // CRC of the payload, written into flags-adjacent trailer
};

// Encodes a frame, appending the payload CRC as the final 8 payload-adjacent bytes of the frame
// body. Fails when the payload exceeds the configured bound.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits);

// Incremental decoder. Bytes are fed in arbitrary chunks; complete frames are produced in order.
// A declared length above the configured bound is rejected before allocation.
class FrameDecoder {
 public:
  explicit FrameDecoder(const Limits& limits) : limits_(limits) {}

  [[nodiscard]] Status feed(const char* data, std::size_t size);
  // Returns the next complete frame, or nullptr when more bytes are needed.
  [[nodiscard]] Result<const Frame*> next();
  void consume();

  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::size_t frames_decoded() const noexcept { return frames_decoded_; }

 private:
  Limits limits_;
  std::vector<char> buffer_{};
  std::size_t cursor_{0};
  Frame current_{};
  bool have_current_{false};
  std::size_t frames_decoded_{0};
};

}  // namespace congestion

#endif  // CONGESTION_TRANSPORT_FRAME_HPP
