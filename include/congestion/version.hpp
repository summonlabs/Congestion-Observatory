// Congestion Observatory - version and build identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_VERSION_HPP
#define CONGESTION_VERSION_HPP

#include <cstdint>
#include <string>

namespace congestion {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// Semantic version string of the runtime, e.g. "1.0.0".
[[nodiscard]] std::string version_string();

// Build identity string (compiler + configuration). Never contains machine-specific paths.
[[nodiscard]] std::string build_identity();

// Stable product identifier used in exported payloads.
[[nodiscard]] std::string_view product_id();

// Persistence format version. Major mismatch is an incompatibility; minor is forward compatible
// only in the sense that older readers reject newer minors (see persistence documentation).
inline constexpr std::uint32_t kSnapshotFormatMajor = 1;
inline constexpr std::uint32_t kSnapshotFormatMinor = 0;

// Wire protocol version for the independent-process transport.
inline constexpr std::uint32_t kProtocolVersion = 1;

}  // namespace congestion

#endif  // CONGESTION_VERSION_HPP
