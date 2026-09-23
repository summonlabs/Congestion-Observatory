// Congestion Observatory - bounded evidence store with fencing and retention.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_EVIDENCE_STORE_HPP
#define CONGESTION_EVIDENCE_STORE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "congestion/core/cancel.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/core/result.hpp"
#include "congestion/evidence/evidence.hpp"
#include "congestion/model/topology.hpp"

namespace congestion {

// Ingestion policy: the rules under which a record is admitted.
struct IngestPolicy {
  AuthorityLevel minimum_authority{AuthorityLevel::kUnknown};
  bool require_known_subject{true};
  bool require_incarnation{true};
  bool reject_unknown_clock{false};
  FreshnessPolicy freshness{};
};

struct IngestOutcome {
  EvidenceId id{};
  bool stored{false};
  FenceDecision fence{FenceDecision::kRejectedUnknownSource};
  std::string detail{};
  Freshness freshness{Freshness::kUnknown};
  bool liveness_reset{false};
  bool sequence_gap{false};
  std::uint64_t evicted{0};
};

struct EvidenceQuery {
  // Unset subject means "any subject"; unset kind means "any kind".
  const EvidenceSubject* subject{nullptr};
  const EvidenceKind* kind{nullptr};
  Timestamp window_start{};
  Timestamp window_end{};
  std::size_t max_records{1024};
  bool include_recovered{true};
  bool include_non_live{true};
  bool newest_first{false};
};

struct StoreStats {
  std::uint64_t accepted{0};
  std::uint64_t rejected_fence{0};
  std::uint64_t rejected_invalid{0};
  std::uint64_t rejected_unknown_subject{0};
  std::uint64_t rejected_authority{0};
  std::uint64_t evicted{0};
  std::uint64_t retention_sweeps{0};
  std::size_t retained{0};
  std::size_t sources{0};
  std::size_t live_sources{0};
};

// The store owns retention, fencing and bounded growth. All mutations hold one mutex; the lock
// is never held while calling into user code, and no nested locks exist (see docs/concurrency.md).
class EvidenceStore {
 public:
  explicit EvidenceStore(const Limits& limits);

  // Admits a record. Structural failures (invalid record, limit violations that cannot be
  // resolved by eviction) return an error; fence rejections return an outcome with stored=false
  // so the caller can count and explain them.
  [[nodiscard]] Result<IngestOutcome> ingest(const EvidenceRecord& record, Timestamp now,
                                             const IngestPolicy& policy,
                                             const Topology* topology = nullptr);

  // Admits a record reconstructed from a snapshot. Fencing is bypassed (the record was already
  // fenced when it was first accepted) but the record is marked recovered_from_snapshot and is
  // therefore never usable as live proof.
  [[nodiscard]] Result<IngestOutcome> ingest_recovered(const EvidenceRecord& record);

  [[nodiscard]] std::vector<EvidenceRecord> query(const EvidenceQuery& query) const;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] StoreStats stats() const;
  [[nodiscard]] std::vector<SourceFenceState> sources() const;
  [[nodiscard]] bool source_live(const SourceId& source) const;
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

  // Retention sweep: removes records older than the supplied horizon. Returns the number of
  // records removed. Bounded by max_records.
  [[nodiscard]] Result<std::size_t> sweep(Timestamp horizon, Timestamp now, std::size_t max_records,
                                          const CancellationToken& token);

  // Liveness is process-local: after a restart every source is unknown until it speaks again.
  void reset_liveness();

 private:
  [[nodiscard]] Result<IngestOutcome> admit_locked(const EvidenceRecord& record, bool recovered,
                                                   std::size_t* evicted_out);

  Limits limits_;
  mutable std::mutex mutex_;
  // Per subject, records in insertion order (oldest first). Ordering is by insertion, not by
  // timestamp, so an out of order arrival can never displace a newer observation.
  std::map<Name, std::vector<EvidenceRecord>> by_subject_{};
  // Global eviction index: insertion index -> subject key. The smallest key is the oldest
  // retained record, which is also the front of that subject's vector.
  std::map<std::uint64_t, Name> insertion_order_{};
  std::map<Name, SourceFenceState> sources_{};
  std::uint64_t next_insertion_index_{1};
  std::size_t retained_{0};
  StoreStats stats_{};
};

}  // namespace congestion

#endif  // CONGESTION_EVIDENCE_STORE_HPP
