// Fabric Efficiency Ledger - JSON codecs for the topology document.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string_view>

#include "fel/serialize.hpp"
#include "fel/topology.hpp"

namespace fel {
namespace detail {

[[nodiscard]] Json topology_to_json(const FabricTopology& topology);
[[nodiscard]] Result<FabricTopology> topology_from_json(const Json& document);
[[nodiscard]] Result<FabricTopology> parse_topology_document(std::string_view text);

}  // namespace detail
}  // namespace fel
