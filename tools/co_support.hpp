// Congestion Observatory - shared command line support utilities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TOOLS_CO_SUPPORT_HPP
#define CONGESTION_TOOLS_CO_SUPPORT_HPP

#include <string>
#include <string_view>
#include <vector>

#include "congestion/congestion.hpp"

namespace congestion::tools {

// Parses a duration such as "500ms", "10s", "2m", "1h". A bare number is interpreted as seconds.
[[nodiscard]] Result<Duration> parse_duration(std::string_view text);

// Parses a wall clock instant: either ISO-8601 UTC or "@seconds" since the Unix epoch.
[[nodiscard]] Result<Timestamp> parse_timestamp(std::string_view text);

// Reads a file into memory with an explicit byte bound.
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::size_t max_bytes);

// Topology document loading. The schema is documented in docs/topology-format.md.
[[nodiscard]] Result<Topology> load_topology_document(std::string_view text, const Limits& limits);
[[nodiscard]] Result<Topology> load_topology_file(const std::string& path, const Limits& limits);

// Evidence document loading. Accepts either a bare JSON array of records or an object with a
// "records" array.
[[nodiscard]] Result<std::vector<EvidenceRecord>> load_evidence_document(std::string_view text,
                                                                        const Limits& limits);
[[nodiscard]] Result<std::vector<EvidenceRecord>> load_evidence_file(const std::string& path,
                                                                    const Limits& limits);

// Renders a topology for humans.
[[nodiscard]] std::string describe_topology(const Topology& topology);

// Formats a verdict line used by the inspect commands.
[[nodiscard]] std::string describe_assessment(const CongestionAssessment& assessment);

}  // namespace congestion::tools

#endif  // CONGESTION_TOOLS_CO_SUPPORT_HPP
