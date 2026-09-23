// Fabric Efficiency Ledger - version identification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string_view>

namespace fel {

inline constexpr std::string_view kProjectName = "Fabric Efficiency Ledger";
inline constexpr std::string_view kLibraryName = "fel";
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// Persisted and serialized format versions. These are part of the on-disk and
// on-wire contract and must only change with an explicit migration path.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::uint32_t kEvidenceFormatVersion = 1;
inline constexpr std::uint32_t kPolicyFormatVersion = 1;
inline constexpr std::uint32_t kTopologyFormatVersion = 1;
inline constexpr std::uint32_t kExportFormatVersion = 1;
inline constexpr std::uint32_t kExplanationFormatVersion = 1;

}  // namespace fel
