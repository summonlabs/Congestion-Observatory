// Congestion Observatory - durable snapshot store with conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_PERSISTENCE_STORE_HPP
#define CONGESTION_PERSISTENCE_STORE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "congestion/core/result.hpp"
#include "congestion/persistence/snapshot.hpp"

namespace congestion {

enum class LoadOutcome : std::uint8_t {
  kLoadedPrimary = 0,       // primary snapshot applied
  kLoadedFallback = 1,      // primary rejected, previous good snapshot applied
  kLoadedNothing = 2,       // no usable snapshot found
};

[[nodiscard]] std::string_view to_string(LoadOutcome outcome) noexcept;

struct LoadReport {
  LoadOutcome outcome{LoadOutcome::kLoadedNothing};
  SnapshotContent content{};
  std::vector<std::string> notes{};   // why a fallback happened, in order
  bool integrity_failure{false};
  bool version_mismatch{false};
};

struct SaveReport {
  std::string primary_path{};
  std::string fallback_path{};
  std::uint64_t bytes_written{0};
  std::uint64_t crc64{0};
};

// Writes through a temporary file and renames into place, keeping the previous snapshot as the
// fallback. A crash therefore leaves either the previous snapshot or the new one, never a
// half-written file.
class SnapshotStore {
 public:
  SnapshotStore(std::string directory, std::string basename, const Limits& limits);

  [[nodiscard]] Result<SaveReport> save(const SnapshotContent& content);

  // Conservative recovery: try the primary, then the fallback. Every rejection is reported.
  [[nodiscard]] Result<LoadReport> load(const ClassificationPolicy& policy) const;

  [[nodiscard]] const std::string& primary_path() const noexcept { return primary_path_; }
  [[nodiscard]] const std::string& fallback_path() const noexcept { return fallback_path_; }

 private:
  std::string directory_{};
  std::string basename_{};
  std::string primary_path_{};
  std::string fallback_path_{};
  Limits limits_{};
  // Save and load touch the same files; the mutex serialises them so that a concurrent reader
  // never observes a half-rotated pair of snapshots.
  mutable std::mutex mutex_{};
};

// Process local identity, used to detect that a snapshot came from a different runtime instance.
[[nodiscard]] std::uint64_t runtime_instance_id() noexcept;

}  // namespace congestion

#endif  // CONGESTION_PERSISTENCE_STORE_HPP
