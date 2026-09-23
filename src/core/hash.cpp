// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/hash.hpp"

namespace congestion {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_hex(std::string& out, std::uint64_t value) {
  char buffer[16];
  for (int i = 15; i >= 0; --i) {
    buffer[i] = kHexDigits[static_cast<std::size_t>(value & 0xFULL)];
    value >>= 4;
  }
  out.append(buffer, sizeof(buffer));
}

}  // namespace

void StableHasher::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t h1 = h1_;
  std::uint64_t h2 = h2_;
  for (std::size_t i = 0; i < size; ++i) {
    h1 ^= static_cast<std::uint64_t>(bytes[i]);
    h1 *= kFnvPrime64;
    h2 ^= static_cast<std::uint64_t>(bytes[i]) + 0x9dU;
    h2 *= kFnvPrime64;
  }
  h1_ = h1;
  h2_ = h2;
  length_ += size;
}

void StableHasher::update_u32(std::uint32_t value) noexcept {
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(value & 0xFFu),
      static_cast<unsigned char>((value >> 8) & 0xFFu),
      static_cast<unsigned char>((value >> 16) & 0xFFu),
      static_cast<unsigned char>((value >> 24) & 0xFFu),
  };
  update(bytes, sizeof(bytes));
}

void StableHasher::update_u64(std::uint64_t value) noexcept {
  const unsigned char bytes[8] = {
      static_cast<unsigned char>(value & 0xFFu),
      static_cast<unsigned char>((value >> 8) & 0xFFu),
      static_cast<unsigned char>((value >> 16) & 0xFFu),
      static_cast<unsigned char>((value >> 24) & 0xFFu),
      static_cast<unsigned char>((value >> 32) & 0xFFu),
      static_cast<unsigned char>((value >> 40) & 0xFFu),
      static_cast<unsigned char>((value >> 48) & 0xFFu),
      static_cast<unsigned char>((value >> 56) & 0xFFu),
  };
  update(bytes, sizeof(bytes));
}

void StableHasher::update_i64(std::int64_t value) noexcept {
  update_u64(static_cast<std::uint64_t>(value));
}

std::uint64_t StableHasher::digest64() const noexcept {
  // Final avalanche so that small inputs do not produce visibly related digests.
  std::uint64_t h = h1_;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

std::array<std::uint64_t, 2> StableHasher::digest128() const noexcept {
  std::uint64_t a = h1_;
  std::uint64_t b = h2_;
  a ^= length_ + 0x9e3779b97f4a7c15ULL;
  b ^= length_ + 0xc2b2ae3d27d4eb4fULL;
  a ^= a >> 33;
  a *= 0xff51afd7ed558ccdULL;
  a ^= a >> 33;
  a *= 0xc4ceb9fe1a85ec53ULL;
  a ^= a >> 33;
  b ^= b >> 31;
  b *= 0x7fb5d329728ea185ULL;
  b ^= b >> 27;
  b *= 0x81dadef4bc2dd44dULL;
  b ^= b >> 33;
  return {a, b};
}

std::uint64_t stable_hash64(std::string_view text) noexcept {
  StableHasher hasher;
  hasher.update(text);
  return hasher.digest64();
}

std::string to_hex(std::uint64_t value) {
  std::string out;
  out.reserve(16);
  append_hex(out, value);
  return out;
}

std::string to_hex(const std::array<std::uint64_t, 2>& value) {
  std::string out;
  out.reserve(32);
  append_hex(out, value[0]);
  append_hex(out, value[1]);
  return out;
}

}  // namespace congestion
