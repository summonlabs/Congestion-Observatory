// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/evidence/store.hpp"

#include <algorithm>

namespace congestion {
namespace {

void sort_records(std::vector<EvidenceRecord>& records, bool newest_first) {
  std::sort(records.begin(), records.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
              if (lhs.observed_at != rhs.observed_at) {
                return lhs.observed_at < rhs.observed_at;
              }
              if (lhs.insertion_index != rhs.insertion_index) {
                return lhs.insertion_index < rhs.insertion_index;
              }
              return lhs.id < rhs.id;
            });
  if (newest_first) {
    std::reverse(records.begin(), records.end());
  }
}

}  // namespace

EvidenceStore::EvidenceStore(const Limits& limits) : limits_(limits) {}

Result<IngestOutcome> EvidenceStore::admit_locked(const EvidenceRecord& record, bool recovered,
                                                  std::size_t* evicted_out) {
  const Name key = Name::unchecked(record.subject.str());
  if (!key.valid()) {
    return make_error(ErrorCode::kInvalidArgument, "evidence subject is not a canonical name",
                      record.subject.str());
  }

  EvidenceRecord stored = record;
  stored.id = compute_evidence_id(record);
  stored.insertion_index = next_insertion_index_;
  ++next_insertion_index_;
  if (recovered) {
    stored.recovered_from_snapshot = true;
  }

  std::size_t evicted = 0;
  if (retained_ >= limits_.max_retained_evidence) {
    // Evict the globally oldest retained record: the smallest insertion index still present.
    const auto oldest = insertion_order_.begin();
    if (oldest != insertion_order_.end()) {
      auto& bucket = by_subject_[oldest->second];
      if (!bucket.empty()) {
        bucket.erase(bucket.begin());
      }
      insertion_order_.erase(oldest);
      if (retained_ > 0) {
        --retained_;
      }
      ++evicted;
      ++stats_.evicted;
    }
  }

  auto& bucket = by_subject_[key];
  if (bucket.size() >= limits_.max_evidence_per_subject) {
    const std::uint64_t evicted_index = bucket.front().insertion_index;
    bucket.erase(bucket.begin());
    insertion_order_.erase(evicted_index);
    if (retained_ > 0) {
      --retained_;
    }
    ++evicted;
    ++stats_.evicted;
  }

  bucket.push_back(stored);
  insertion_order_.emplace(stored.insertion_index, key);
  ++retained_;
  if (evicted_out != nullptr) {
    *evicted_out = evicted;
  }
  return Result<IngestOutcome>(IngestOutcome{});
}

Result<IngestOutcome> EvidenceStore::ingest(const EvidenceRecord& record, Timestamp now,
                                            const IngestPolicy& policy, const Topology* topology) {
  const Status valid = validate_evidence(record, limits_);
  if (!valid.ok()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rejected_invalid;
    return valid.error();
  }
  if (policy.require_known_subject && topology != nullptr) {
    const Status present = topology->require(record.subject);
    if (!present.ok()) {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.rejected_unknown_subject;
      return present.error();
    }
  }
  if (policy.require_incarnation && record.fence.incarnation.is_zero()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rejected_invalid;
    return make_error(ErrorCode::kInvalidArgument, "evidence carries no source incarnation",
                      record.provenance.source.str());
  }

  const std::lock_guard<std::mutex> lock(mutex_);

  const Name source_key = record.provenance.source.name();
  auto source_it = sources_.find(source_key);
  if (source_it == sources_.end() && sources_.size() >= limits_.max_sources) {
    ++stats_.rejected_invalid;
    return make_error(ErrorCode::kLimitExceeded, "source limit reached", source_key.str());
  }

  const SourceFenceState previous =
      source_it == sources_.end() ? SourceFenceState{} : source_it->second;
  const FenceOutcome fence = evaluate_fence(record.fence, record.provenance.authority,
                                            policy.minimum_authority, previous);

  IngestOutcome outcome;
  outcome.id = compute_evidence_id(record);
  outcome.fence = fence.decision;
  outcome.detail = fence.detail;
  outcome.liveness_reset = fence.liveness_reset;
  outcome.sequence_gap = fence.sequence_gap;

  if (!fence.accepted()) {
    ++stats_.rejected_fence;
    auto& state = sources_[source_key];
    state.source = record.provenance.source;
    state.rejected += 1;
    outcome.stored = false;
    return outcome;
  }

  if (fence.liveness_reset) {
    // A new incarnation retires the previous one: its evidence stays as history but can no
    // longer support a current verdict.
    for (auto& entry : by_subject_) {
      for (EvidenceRecord& existing : entry.second) {
        if (existing.provenance.source.name() == source_key) {
          existing.retired = true;
        }
      }
    }
  }

  std::size_t evicted = 0;
  auto admitted = admit_locked(record, false, &evicted);
  if (!admitted.ok()) {
    return admitted.error();
  }
  outcome.evicted = evicted;
  outcome.stored = true;
  outcome.freshness = assess_freshness(record, now, policy.freshness).freshness;

  auto& state = sources_[source_key];
  state.source = record.provenance.source;
  state.last = record.fence;
  state.last_seen = now;
  state.last_authority = record.provenance.authority;
  state.live = true;
  state.has_state = true;
  state.accepted += 1;

  ++stats_.accepted;
  return outcome;
}

Result<IngestOutcome> EvidenceStore::ingest_recovered(const EvidenceRecord& record) {
  const Status valid = validate_evidence(record, limits_);
  if (!valid.ok()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rejected_invalid;
    return valid.error();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  const Name source_key = record.provenance.source.name();
  auto source_it = sources_.find(source_key);
  if (source_it == sources_.end()) {
    if (sources_.size() >= limits_.max_sources) {
      ++stats_.rejected_invalid;
      return make_error(ErrorCode::kLimitExceeded, "source limit reached", source_key.str());
    }
    // Recovered sources are known but not live: nothing was heard from them in this process.
    SourceFenceState state;
    state.source = record.provenance.source;
    state.live = false;
    state.has_state = false;
    sources_.emplace(source_key, state);
  }

  std::size_t evicted = 0;
  auto admitted = admit_locked(record, true, &evicted);
  if (!admitted.ok()) {
    return admitted.error();
  }
  IngestOutcome outcome;
  outcome.id = compute_evidence_id(record);
  outcome.stored = true;
  outcome.fence = FenceDecision::kAcceptedFirstObservation;
  outcome.detail = "recovered from snapshot";
  outcome.freshness = Freshness::kUnknown;
  outcome.evicted = evicted;
  return outcome;
}

std::vector<EvidenceRecord> EvidenceStore::query(const EvidenceQuery& query) const {
  std::vector<EvidenceRecord> out;
  const std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t budget = query.max_records == 0 ? 0 : query.max_records;

  // A query whose window is left at the zero value means "the whole retention horizon".
  const bool unbounded_window = query.window_start.is_zero() && query.window_end.is_zero();
  const auto consider = [&](const EvidenceRecord& record) {
    if (out.size() >= budget) {
      return;
    }
    if (query.kind != nullptr && record.kind != *query.kind) {
      return;
    }
    if (!unbounded_window &&
        (record.observed_at < query.window_start || record.observed_at > query.window_end)) {
      return;
    }
    if (!query.include_recovered && record.recovered_from_snapshot) {
      return;
    }
    if (!query.include_non_live && (record.retired || record.recovered_from_snapshot)) {
      return;
    }
    out.push_back(record);
  };

  if (query.subject != nullptr) {
    const auto it = by_subject_.find(Name::unchecked(query.subject->str()));
    if (it != by_subject_.end()) {
      for (const EvidenceRecord& record : it->second) {
        consider(record);
      }
    }
  } else {
    for (const auto& entry : by_subject_) {
      for (const EvidenceRecord& record : entry.second) {
        consider(record);
      }
    }
  }
  sort_records(out, query.newest_first);
  return out;
}

std::size_t EvidenceStore::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return retained_;
}

StoreStats EvidenceStore::stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  StoreStats out = stats_;
  out.retained = retained_;
  out.sources = sources_.size();
  std::size_t live = 0;
  for (const auto& entry : sources_) {
    if (entry.second.live) {
      ++live;
    }
  }
  out.live_sources = live;
  return out;
}

std::vector<SourceFenceState> EvidenceStore::sources() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SourceFenceState> out;
  out.reserve(sources_.size());
  for (const auto& entry : sources_) {
    out.push_back(entry.second);
  }
  return out;
}

bool EvidenceStore::source_live(const SourceId& source) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sources_.find(source.name());
  return it != sources_.end() && it->second.live;
}

Result<std::size_t> EvidenceStore::sweep(Timestamp horizon, Timestamp now, std::size_t max_records,
                                         const CancellationToken& token) {
  (void)now;
  if (max_records == 0) {
    return std::size_t{0};
  }
  if (token.cancelled()) {
    // Cancellation is observed before any work is done, not only between removals.
    return make_error(ErrorCode::kCancelled, "retention sweep cancelled");
  }
  std::size_t removed = 0;
  const std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.retention_sweeps;
  for (auto& entry : by_subject_) {
    auto& bucket = entry.second;
    std::size_t write = 0;
    for (std::size_t read = 0; read < bucket.size(); ++read) {
      if (removed >= max_records) {
        // Keep the remaining records untouched and stop; the caller may sweep again.
        for (std::size_t rest = read; rest < bucket.size(); ++rest) {
          bucket[write++] = std::move(bucket[rest]);
        }
        bucket.resize(write);
        break;
      }
      if (token.cancelled()) {
        for (std::size_t rest = read; rest < bucket.size(); ++rest) {
          bucket[write++] = std::move(bucket[rest]);
        }
        bucket.resize(write);
        return make_error(ErrorCode::kCancelled, "retention sweep cancelled");
      }
      if (bucket[read].observed_at < horizon) {
        insertion_order_.erase(bucket[read].insertion_index);
        if (retained_ > 0) {
          --retained_;
        }
        ++removed;
      } else {
        bucket[write++] = std::move(bucket[read]);
      }
    }
    if (write < bucket.size()) {
      bucket.resize(write);
    }
  }
  return removed;
}

void EvidenceStore::reset_liveness() {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : sources_) {
    entry.second.live = false;
  }
}

}  // namespace congestion
