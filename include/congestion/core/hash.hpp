// Congestion Observatory - deterministic, platform independent hashing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_HASH_HPP
#define CONGESTION_CORE_HASH_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace congestion {

// FNV-1a is used for deterministic identity derivation (episode ids, evidence ids, digests).
// It is deliberately NOT a cryptographic hash and is never used for authentication. Its only
// required properties here are: byte-exact reproducibility across platforms and builds, plus
// enough dispersion that distinct canonical encodings collide with negligible probability in
// the bounded populations this runtime admits.
class StableHasher {
 public:
  static constexpr std::uint64_t kFnvOffset64 = 0xcbf29ce484222325ULL;
  static constexpr std::uint64_t kFnvPrime64 = 0x100000001b3ULL;
  static constexpr std::uint64_t kFnvOffset32 = 0x811c9dc5ULL;
  static constexpr std::uint64_t kFnvPrime32 = 0x01000193ULL;

  constexpr StableHasher() noexcept = default;

  void reset() noexcept {
    h1_ = kFnvOffset64;
    h2_ = kFnvOffset64 ^ 0x9e3779b97f4a7c15ULL;
    length_ = 0;
  }

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u8(std::uint8_t value) noexcept { update(&value, 1); }
  void update_u32(std::uint32_t value) noexcept;  // explicit little endian
  void update_u64(std::uint64_t value) noexcept;  // explicit little endian
  void update_i64(std::int64_t value) noexcept;
  void update_bool(bool value) noexcept { update_u8(value ? 1U : 0U); }

  // Field separator: makes concatenated encodings unambiguous.
  void separator() noexcept { update_u8(0x1f); }

  [[nodiscard]] std::uint64_t digest64() const noexcept;
  [[nodiscard]] std::array<std::uint64_t, 2> digest128() const noexcept;
  [[nodiscard]] std::size_t length() const noexcept { return length_; }

 private:
  std::uint64_t h1_{kFnvOffset64};
  std::uint64_t h2_{kFnvOffset64 ^ 0x9e3779b97f4a7c15ULL};
  std::size_t length_{0};
};

// Convenience: 64 bit digest of a byte range.
[[nodiscard]] std::uint64_t stable_hash64(std::string_view text) noexcept;

// Hex rendering used for ids and digests. Lower case, no prefix, fixed width.
[[nodiscard]] std::string to_hex(std::uint64_t value);
[[nodiscard]] std::string to_hex(const std::array<std::uint64_t, 2>& value);

}  // namespace congestion

#endif  // CONGESTION_CORE_HASH_HPP
