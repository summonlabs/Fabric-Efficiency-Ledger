// Fabric Efficiency Ledger - document input and output helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "fel/observation.hpp"
#include "fel/serialize.hpp"
#include "fel/topology.hpp"

namespace fel {

// Decodes an evidence document. Two shapes are accepted:
//   * JSON Lines, one observation object per line
//   * a single envelope object with a format_version and an observations array
// Anything else is refused with a specific error code; the decoder never
// guesses at a partially valid document.
[[nodiscard]] Result<ObservationBatch> parse_evidence_document(std::string_view text,
                                                               std::string_view origin_label);

[[nodiscard]] Json observation_to_json(const Observation& observation);
[[nodiscard]] Result<Observation> observation_from_json(const Json& document);

[[nodiscard]] Json topology_to_json(const FabricTopology& topology);
[[nodiscard]] Result<FabricTopology> topology_from_json(const Json& document);
[[nodiscard]] Result<FabricTopology> parse_topology_document(std::string_view text);

// Bounded file helpers. The byte budget is enforced while reading, so a huge
// file cannot be pulled into memory before the limit is applied.
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::uint64_t max_bytes);
[[nodiscard]] Result<void> write_text_file(const std::string& path, std::string_view content);

}  // namespace fel
