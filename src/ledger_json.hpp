// Fabric Efficiency Ledger - JSON codecs for persisted ledger records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string_view>

#include "fel/claim.hpp"
#include "fel/observation.hpp"
#include "fel/period.hpp"
#include "fel/serialize.hpp"

namespace fel {
namespace detail {

// Evidence codecs. These are the on-disk and on-wire contract for a single
// observation; the format version is carried on the batch document.
[[nodiscard]] Json observation_to_json(const Observation& observation);
[[nodiscard]] Result<Observation> observation_from_json(const Json& document);

// Decodes either a single JSON object or a {"observations": [...]} document.
[[nodiscard]] Result<ObservationBatch> parse_evidence_document(std::string_view text,
                                                               std::string_view origin_label);

// Period lifecycle records.
[[nodiscard]] Json period_to_json(const AccountingPeriod& period);
[[nodiscard]] Result<AccountingPeriod> period_from_json(const Json& document);

[[nodiscard]] Json revision_to_json(const PeriodRevision& revision, bool include_evidence);
[[nodiscard]] Result<PeriodRevision> revision_from_json(const Json& document);

[[nodiscard]] Json correction_to_json(const CorrectionRecord& record);
[[nodiscard]] Result<CorrectionRecord> correction_from_json(const Json& document);

// Requested-or-persisted selection helper used by the ledger on open.
[[nodiscard]] Result<Json> parse_json_text(std::string_view text, std::string_view what);

}  // namespace detail
}  // namespace fel
