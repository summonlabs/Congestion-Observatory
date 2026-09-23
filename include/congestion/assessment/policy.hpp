// Congestion Observatory - versioned, deterministic classification policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_ASSESSMENT_POLICY_HPP
#define CONGESTION_ASSESSMENT_POLICY_HPP

#include <cstdint>
#include <string>

#include "congestion/core/hash.hpp"
#include "congestion/evidence/evidence.hpp"

namespace congestion {

// Every threshold the runtime uses to turn evidence into a verdict lives here. The policy is
// versioned, digestible and persisted with each snapshot; two runtimes with different policy
// digests refuse to continue each other's episode history.
struct ClassificationPolicy {
  // Identity of this policy. Change it whenever a threshold or rule changes meaning.
  std::string version{"co-policy-1"};

  // Utilization is an input to interpretation, never a verdict by itself.
  double utilization_attention{0.70};
  double utilization_high{0.90};

  // Pressure: backlog relative to the queue/buffer capacity reported by the source.
  double pressure_occupancy_ratio{0.60};
  double pressure_depth_growth_ratio{1.50};

  // Impairment rates are expressed per offered unit over the evaluation window.
  double drop_rate_threshold{1e-6};
  double mark_rate_threshold{1e-4};
  double pause_rate_threshold{1e-5};
  double retransmit_rate_threshold{1e-5};
  double latency_inflation_factor{2.00};

  // Saturation: offered demand relative to serviceable capacity.
  double demand_capacity_ratio{1.00};

  // Agreement analysis.
  double conflict_relative_tolerance{0.10};
  std::size_t corroborating_source_target{2};

  // Windows.
  Duration evaluation_window{Duration::from_seconds(10)};
  Duration pressure_window{Duration::from_seconds(5)};
  Duration impairment_window{Duration::from_seconds(10)};

  FreshnessPolicy freshness{};

  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string describe() const;
  [[nodiscard]] Status validate() const;
};

}  // namespace congestion

#endif  // CONGESTION_ASSESSMENT_POLICY_HPP
