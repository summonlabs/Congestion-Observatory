// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/crc64.hpp"

namespace congestion {

void Crc64::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t crc = state_;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<std::uint64_t>(bytes[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint64_t mask = 0ULL - (crc & 1ULL);
      crc = (crc >> 1) ^ (kPolynomialReflected & mask);
    }
  }
  state_ = crc;
}

std::uint64_t crc64(std::string_view text) noexcept {
  Crc64 instance;
  instance.update(text);
  return instance.value();
}

}  // namespace congestion
