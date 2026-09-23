// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/assessment/policy.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace congestion {
namespace {

void hash_double(StableHasher& hasher, double value) {
  // Hash the exact bit pattern so that two policies that differ in the last bit are distinct.
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64 bit");
  std::memcpy(&bits, &value, sizeof(bits));
  hasher.update_u64(bits);
}

}  // namespace

std::uint64_t ClassificationPolicy::digest() const noexcept {
  StableHasher hasher;
  hasher.update(version);
  hasher.separator();
  hash_double(hasher, utilization_attention);
  hash_double(hasher, utilization_high);
  hash_double(hasher, pressure_occupancy_ratio);
  hash_double(hasher, pressure_depth_growth_ratio);
  hash_double(hasher, drop_rate_threshold);
  hash_double(hasher, mark_rate_threshold);
  hash_double(hasher, pause_rate_threshold);
  hash_double(hasher, retransmit_rate_threshold);
  hash_double(hasher, latency_inflation_factor);
  hash_double(hasher, demand_capacity_ratio);
  hash_double(hasher, conflict_relative_tolerance);
  hasher.update_u64(corroborating_source_target);
  hasher.update_i64(evaluation_window.nanos());
  hasher.update_i64(pressure_window.nanos());
  hasher.update_i64(impairment_window.nanos());
  hasher.update_i64(freshness.fresh_within.nanos());
  hasher.update_i64(freshness.aging_within.nanos());
  hasher.update_i64(freshness.stale_within.nanos());
  hasher.update_i64(freshness.max_future_skew.nanos());
  hasher.update_bool(freshness.require_plausible_timestamps);
  return hasher.digest64();
}

std::string ClassificationPolicy::describe() const {
  std::string out;
  const auto add = [&out](const char* name, const std::string& value) {
    out += name;
    out += "=";
    out += value;
    out += "\n";
  };
  add("version", version);
  add("utilization_attention", std::to_string(utilization_attention));
  add("utilization_high", std::to_string(utilization_high));
  add("pressure_occupancy_ratio", std::to_string(pressure_occupancy_ratio));
  add("pressure_depth_growth_ratio", std::to_string(pressure_depth_growth_ratio));
  add("drop_rate_threshold", std::to_string(drop_rate_threshold));
  add("mark_rate_threshold", std::to_string(mark_rate_threshold));
  add("pause_rate_threshold", std::to_string(pause_rate_threshold));
  add("retransmit_rate_threshold", std::to_string(retransmit_rate_threshold));
  add("latency_inflation_factor", std::to_string(latency_inflation_factor));
  add("demand_capacity_ratio", std::to_string(demand_capacity_ratio));
  add("conflict_relative_tolerance", std::to_string(conflict_relative_tolerance));
  add("corroborating_source_target", std::to_string(corroborating_source_target));
  add("evaluation_window", evaluation_window.to_string());
  add("pressure_window", pressure_window.to_string());
  add("impairment_window", impairment_window.to_string());
  add("fresh_within", freshness.fresh_within.to_string());
  add("aging_within", freshness.aging_within.to_string());
  add("stale_within", freshness.stale_within.to_string());
  add("max_future_skew", freshness.max_future_skew.to_string());
  add("require_plausible_timestamps", freshness.require_plausible_timestamps ? "true" : "false");
  add("digest", to_hex(digest()));
  if (!out.empty()) {
    out.pop_back();
  }
  return out;
}

Status ClassificationPolicy::validate() const {
  if (version.empty()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "classification policy has no version"));
  }
  const double ratios[] = {utilization_attention,          utilization_high,
                           pressure_occupancy_ratio,       latency_inflation_factor,
                           demand_capacity_ratio,          conflict_relative_tolerance};
  const char* ratio_names[] = {"utilization_attention",    "utilization_high",
                               "pressure_occupancy_ratio", "latency_inflation_factor",
                               "demand_capacity_ratio",    "conflict_relative_tolerance"};
  for (std::size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); ++i) {
    if (!std::isfinite(ratios[i]) || ratios[i] < 0.0) {
      return Status(make_error(ErrorCode::kInvalidArgument,
                               std::string("policy threshold must be finite and non negative: ") +
                                   ratio_names[i]));
    }
  }
  if (utilization_attention > utilization_high) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "utilization_attention must not exceed utilization_high"));
  }
  if (conflict_relative_tolerance > 1.0) {
    return Status(make_error(ErrorCode::kOutOfRange,
                             "conflict_relative_tolerance must not exceed 1.0"));
  }
  if (corroborating_source_target == 0) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "corroborating_source_target must be at least one"));
  }
  if (freshness.fresh_within <= Duration::zero()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "fresh_within must be positive"));
  }
  if (freshness.aging_within < freshness.fresh_within) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "aging_within must not be shorter than fresh_within"));
  }
  if (freshness.stale_within < freshness.aging_within) {
    return Status(make_error(ErrorCode::kInvalidArgument,
                             "stale_within must not be shorter than aging_within"));
  }
  if (evaluation_window <= Duration::zero()) {
    return Status(make_error(ErrorCode::kInvalidArgument, "evaluation_window must be positive"));
  }
  return Status{};
}

}  // namespace congestion
