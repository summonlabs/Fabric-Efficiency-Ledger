// Fabric Efficiency Ledger - document input and output helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/io.hpp"

#include <cstdio>
#include <string>

#include "ledger_json.hpp"
#include "topology_json.hpp"

namespace fel {

Result<ObservationBatch> parse_evidence_document(std::string_view text,
                                                 std::string_view origin_label) {
  return detail::parse_evidence_document(text, origin_label);
}

Json observation_to_json(const Observation& observation) {
  return detail::observation_to_json(observation);
}

Result<Observation> observation_from_json(const Json& document) {
  return detail::observation_from_json(document);
}

Json topology_to_json(const FabricTopology& topology) {
  return detail::topology_to_json(topology);
}

Result<FabricTopology> topology_from_json(const Json& document) {
  return detail::topology_from_json(document);
}

Result<FabricTopology> parse_topology_document(std::string_view text) {
  return detail::parse_topology_document(text);
}

Result<std::string> read_text_file(const std::string& path, std::uint64_t max_bytes) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot open file for reading", path);
  }
  std::string text;
  char buffer[8192];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
    if (static_cast<std::uint64_t>(text.size()) > max_bytes) {
      std::fclose(file);
      return make_error(ErrorCode::PayloadTooLarge, "file exceeds the read budget", path);
    }
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    return make_error(ErrorCode::IoFailure, "read failure", path);
  }
  return text;
}

Result<void> write_text_file(const std::string& path, std::string_view content) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return make_error(ErrorCode::IoFailure, "cannot open file for writing", path);
  }
  const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
  const bool flushed = written == content.size() && std::fflush(file) == 0;
  std::fclose(file);
  if (!flushed) {
    return make_error(ErrorCode::IoFailure, "write failure", path);
  }
  return fel::ok();
}

}  // namespace fel
