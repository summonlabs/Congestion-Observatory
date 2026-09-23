// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/persistence/store.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "congestion/core/checked.hpp"
#include "congestion/core/crc64.hpp"
#include "congestion/core/time.hpp"
#include "congestion/version.hpp"

namespace congestion {
namespace {

std::string join_path(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const std::filesystem::path path = std::filesystem::path(directory) / name;
  return path.string();
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return make_error(ErrorCode::kNotFound, "snapshot file is not readable", path);
  }
  if (size > max_bytes) {
    return make_error(ErrorCode::kLimitExceeded, "snapshot file exceeds the configured bound", path);
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::kIoError, "snapshot file could not be opened", path);
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
      return make_error(ErrorCode::kIoError, "snapshot file was truncated while reading", path);
    }
  }
  return bytes;
}

Status write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Status(make_error(ErrorCode::kIoError, "snapshot temporary file could not be created",
                               temporary));
    }
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
      return Status(make_error(ErrorCode::kIoError, "snapshot write failed", temporary));
    }
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  error.clear();
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return Status(make_error(ErrorCode::kIoError, "snapshot could not be moved into place", path));
  }
  return Status{};
}

}  // namespace

std::string_view to_string(LoadOutcome outcome) noexcept {
  switch (outcome) {
    case LoadOutcome::kLoadedPrimary: return "loaded_primary";
    case LoadOutcome::kLoadedFallback: return "loaded_fallback";
    case LoadOutcome::kLoadedNothing: return "loaded_nothing";
  }
  return "loaded_nothing";
}

std::uint64_t runtime_instance_id() noexcept {
  static const std::uint64_t instance = []() {
    StableHasher hasher;
    hasher.update(build_identity());
    hasher.separator();
    hasher.update_i64(monotonic_nanos());
    hasher.separator();
    static std::atomic<std::uint64_t> counter{0};
    hasher.update_u64(counter.fetch_add(1) + 1);
    std::uint64_t value = hasher.digest64();
    return value == 0 ? 1 : value;
  }();
  return instance;
}

SnapshotStore::SnapshotStore(std::string directory, std::string basename, const Limits& limits)
    : directory_(std::move(directory)),
      basename_(std::move(basename)),
      limits_(limits) {
  primary_path_ = join_path(directory_, basename_ + ".snapshot");
  fallback_path_ = join_path(directory_, basename_ + ".snapshot.previous");
}

Result<SaveReport> SnapshotStore::save(const SnapshotContent& content) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto encoded = encode_snapshot(content, limits_);
  if (!encoded.ok()) {
    return encoded.error();
  }
  if (!directory_.empty()) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
      return make_error(ErrorCode::kIoError, "state directory could not be created", directory_);
    }
  }

  // Keep the previous good snapshot as the fallback before replacing the primary.
  std::error_code error;
  if (std::filesystem::exists(primary_path_, error) && !error) {
    std::filesystem::remove(fallback_path_, error);
    error.clear();
    std::filesystem::copy_file(primary_path_, fallback_path_,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      return make_error(ErrorCode::kIoError, "previous snapshot could not be retained as fallback",
                        fallback_path_);
    }
  }

  const Status written = write_file_atomic(primary_path_, encoded.value());
  if (!written.ok()) {
    return written.error();
  }

  SaveReport report;
  report.primary_path = primary_path_;
  report.fallback_path = fallback_path_;
  report.bytes_written = encoded.value().size();
  report.crc64 = crc64(std::string_view(reinterpret_cast<const char*>(encoded.value().data()),
                                        encoded.value().size()));
  return report;
}

Result<LoadReport> SnapshotStore::load(const ClassificationPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  LoadReport report;
  std::error_code error;
  if (std::filesystem::exists(primary_path_, error) && !error) {
    auto bytes = read_file(primary_path_, limits_.max_snapshot_bytes);
    if (bytes.ok()) {
      auto decoded = decode_snapshot(bytes.value(), limits_, policy);
      if (decoded.ok()) {
        report.outcome = LoadOutcome::kLoadedPrimary;
        report.content = std::move(decoded.value());
        return report;
      }
      report.notes.push_back(std::string("primary rejected: ") + decoded.error().describe());
      report.integrity_failure = decoded.error().code() == ErrorCode::kIntegrityFailure;
      report.version_mismatch = decoded.error().code() == ErrorCode::kVersionMismatch;
    } else {
      report.notes.push_back(std::string("primary unreadable: ") + bytes.error().describe());
      report.integrity_failure = true;
    }
  } else {
    report.notes.push_back("no primary snapshot present");
  }

  error.clear();
  if (std::filesystem::exists(fallback_path_, error) && !error) {
    auto bytes = read_file(fallback_path_, limits_.max_snapshot_bytes);
    if (bytes.ok()) {
      auto decoded = decode_snapshot(bytes.value(), limits_, policy);
      if (decoded.ok()) {
        report.outcome = LoadOutcome::kLoadedFallback;
        report.content = std::move(decoded.value());
        report.notes.push_back("fallback snapshot applied");
        return report;
      }
      report.notes.push_back(std::string("fallback rejected: ") + decoded.error().describe());
      report.integrity_failure = true;
    } else {
      report.notes.push_back(std::string("fallback unreadable: ") + bytes.error().describe());
      report.integrity_failure = true;
    }
  } else {
    report.notes.push_back("no fallback snapshot present");
  }

  report.outcome = LoadOutcome::kLoadedNothing;
  return report;
}

}  // namespace congestion
