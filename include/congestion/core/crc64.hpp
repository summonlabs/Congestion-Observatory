// Congestion Observatory - integrity checking for persisted payloads.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_CRC64_HPP
#define CONGESTION_CORE_CRC64_HPP

#include <cstdint>
#include <string_view>

namespace congestion {

// CRC-64/XZ parameters: polynomial 0x42F0E1EBA9EA3693 reflected (0xC96C5795D7870F42),
// init 0xFFFFFFFFFFFFFFFF, reflected input/output, final xor 0xFFFFFFFFFFFFFFFF.
// Implemented bitwise so the value never depends on a table layout, host endianness or
// compiler vendor.
class Crc64 {
 public:
  static constexpr std::uint64_t kInit = 0xFFFFFFFFFFFFFFFFULL;
  static constexpr std::uint64_t kXorOut = 0xFFFFFFFFFFFFFFFFULL;
  static constexpr std::uint64_t kPolynomialReflected = 0xC96C5795D7870F42ULL;

  constexpr Crc64() noexcept = default;

  void reset() noexcept { state_ = kInit; }
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u8(std::uint8_t value) noexcept { update(&value, 1); }

  [[nodiscard]] std::uint64_t value() const noexcept { return state_ ^ kXorOut; }

 private:
  std::uint64_t state_{kInit};
};

[[nodiscard]] std::uint64_t crc64(std::string_view text) noexcept;

}  // namespace congestion

#endif  // CONGESTION_CORE_CRC64_HPP
