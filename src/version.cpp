// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/version.hpp"

#include <string>

namespace congestion {
namespace {

constexpr const char* kProductId = "congestion-observatory";

// Compiler identity, captured at build time. No machine-specific path is embedded.
#if defined(_MSC_VER)
constexpr const char* kCompilerTag = "msvc";
#elif defined(__clang__)
constexpr const char* kCompilerTag = "clang";
#elif defined(__GNUC__)
constexpr const char* kCompilerTag = "gcc";
#else
constexpr const char* kCompilerTag = "unknown";
#endif

#if defined(NDEBUG)
constexpr const char* kBuildKind = "release";
#else
constexpr const char* kBuildKind = "debug";
#endif

#if defined(CONGESTION_ASAN_ENABLED)
constexpr const char* kSanitizerTag = "asan";
#else
constexpr const char* kSanitizerTag = "none";
#endif

}  // namespace

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

std::string build_identity() {
  std::string identity = kProductId;
  identity += "/";
  identity += version_string();
  identity += " ";
  identity += kCompilerTag;
  identity += "-";
  identity += kBuildKind;
  identity += " sanitizer=";
  identity += kSanitizerTag;
  identity += " cxx20";
  return identity;
}

std::string_view product_id() { return kProductId; }

}  // namespace congestion
