// Fabric Efficiency Ledger - command line tool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "fel/io.hpp"
#include "fel/ledger.hpp"
#include "fel/persistence.hpp"
#include "fel/serialize.hpp"
#include "fel/version.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace fel_cli {
namespace {

using fel::ErrorCode;
using fel::Result;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct Options {
  std::map<std::string, std::string> values;
  std::vector<std::string> flags;

  [[nodiscard]] bool has(const std::string& key) const {
    return values.find(key) != values.end() ||
           std::find(flags.begin(), flags.end(), key) != flags.end();
  }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  [[nodiscard]] std::uint64_t get_u64(const std::string& key, std::uint64_t fallback) const {
    const auto it = values.find(key);
    if (it == values.end()) {
      return fallback;
    }
    try {
      return static_cast<std::uint64_t>(std::stoull(it->second));
    } catch (const std::exception&) {
      return fallback;
    }
  }
};

void print_usage() {
  std::printf(
      "Fabric Efficiency Ledger %s\n"
      "\n"
      "usage: fel <command> [options]\n"
      "\n"
      "commands:\n"
      "  init        --store DIR [--policy F] [--topology F] --period-start T --period-end T\n"
      "  ingest      --store DIR --evidence FILE [--period HEX] [--parallel]\n"
      "  close       --store DIR --period HEX --closed-at T [--reason R] [--operator N] [--dry-run]\n"
      "  correct     --store DIR --period HEX --created-at T [--reason R]\n"
      "  query       --store DIR --period HEX [--revision N] [--scope HEX] [--descendants]\n"
      "  aggregate   --store DIR --period HEX --axis AXIS [--window-ms N]\n"
      "  reconcile   --store DIR --period HEX\n"
      "  explain     --store DIR --period HEX --identity HEX\n"
      "  export      --store DIR --period HEX [--format jsonl|csv|canonical-json] [--out FILE]\n"
      "  verify      --store DIR\n"
      "  stats       --store DIR\n"
      "  compact     --store DIR [--dry-run]\n"
      "  lock-hold   --store DIR [--hold-lock-until-eof] [--lock-hold-max-ms N]\n"
      "  version\n"
      "\n"
      "global options: --json  --quiet  --as-of T  --lock-timeout-ms N\n"
      "\n"
      "Exit codes: 0 success, 1 runtime failure, 2 usage error.\n",
      std::string(fel::kVersionString).c_str());
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options* options) {
  for (int i = 0; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--", 0) != 0) {
      return false;
    }
    const std::size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      options->values[argument.substr(0, equals)] = argument.substr(equals + 1);
      continue;
    }
    if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
      options->values[argument] = argv[++i];
      continue;
    }
    options->flags.push_back(argument);
  }
  return true;
}

[[nodiscard]] Result<fel::TimePoint> required_time(const Options& options, const char* key) {
  if (!options.has(key)) {
    return fel::make_error(ErrorCode::MissingRequiredField,
                           std::string("missing required option ") + key);
  }
  return fel::parse_rfc3339(options.get(key));
}

[[nodiscard]] Result<fel::PeriodId> required_period(const Options& options) {
  if (!options.has("--period")) {
    return fel::make_error(ErrorCode::MissingRequiredField, "missing required option --period");
  }
  return fel::PeriodId::parse(options.get("--period"));
}

[[nodiscard]] Result<std::string> read_text_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return fel::make_error(ErrorCode::IoFailure, "cannot open file", path);
  }
  std::string text;
  char buffer[8192];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
    if (text.size() > fel::limits::kMaxRecordBytes * 64U) {
      std::fclose(file);
      return fel::make_error(ErrorCode::PayloadTooLarge, "file exceeds the read budget", path);
    }
  }
  std::fclose(file);
  return text;
}

[[nodiscard]] Result<std::string> read_stdin() {
  std::string text;
  char buffer[8192];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
    text.append(buffer, read);
    if (text.size() > fel::limits::kMaxRecordBytes * 64U) {
      return fel::make_error(ErrorCode::PayloadTooLarge, "stdin exceeds the read budget");
    }
  }
  return text;
}

void report_error(const fel::Error& error) {
  std::fprintf(stderr, "error: %s: %s\n", std::string(fel::error_code_name(error.code)).c_str(),
               error.message.c_str());
  if (!error.detail.empty()) {
    std::fprintf(stderr, "detail: %s\n", error.detail.c_str());
  }
}

[[nodiscard]] fel::OpenOptions make_open_options(const Options& options, bool with_lock) {
  fel::OpenOptions open;
  open.store_path = options.get("--store");
  open.mode = fel::StoreOpenMode::OpenExisting;
  open.take_store_lock = with_lock;
  open.lock_timeout_ms = static_cast<std::uint32_t>(options.get_u64("--lock-timeout-ms", 0));
  if (options.has("--policy")) {
    auto policy = fel::load_policy_file(options.get("--policy"));
    if (policy.has_value()) {
      open.policy = policy.value();
    }
  }
  if (options.has("--topology")) {
    auto text = read_text_file(options.get("--topology"));
    if (text.has_value()) {
      auto topology = fel::parse_topology_document(text.value());
      if (topology.has_value()) {
        open.topology = std::move(topology.value());
      }
    }
  }
  if (options.has("--as-of")) {
    auto when = fel::parse_rfc3339(options.get("--as-of"));
    if (when.has_value()) {
      open.as_of = when.value();
      open.rebase_freshness_on_open = false;
    }
  }
  return open;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
[[nodiscard]] int command_init(const Options& options) {
  if (!options.has("--store")) {
    std::fprintf(stderr, "error: init requires --store\n");
    return kExitUsage;
  }
  auto start = required_time(options, "--period-start");
  if (!start.has_value()) {
    report_error(start.error());
    return kExitUsage;
  }
  auto end = required_time(options, "--period-end");
  if (!end.has_value()) {
    report_error(end.error());
    return kExitUsage;
  }
  fel::OpenOptions open;
  open.store_path = options.get("--store");
  open.mode = fel::StoreOpenMode::OpenOrCreate;
  open.lock_timeout_ms = static_cast<std::uint32_t>(options.get_u64("--lock-timeout-ms", 0));
  if (options.has("--policy")) {
    auto policy = fel::load_policy_file(options.get("--policy"));
    if (!policy.has_value()) {
      report_error(policy.error());
      return kExitFailure;
    }
    open.policy = policy.value();
  }
  if (options.has("--topology")) {
    auto text = read_text_file(options.get("--topology"));
    if (!text.has_value()) {
      report_error(text.error());
      return kExitFailure;
    }
    auto topology = fel::parse_topology_document(text.value());
    if (!topology.has_value()) {
      report_error(topology.error());
      return kExitFailure;
    }
    open.topology = std::move(topology.value());
  }
  auto when = required_time(options, "--as-of");
  if (when.has_value()) {
    open.as_of = when.value();
    open.rebase_freshness_on_open = false;
  } else {
    open.as_of = end.value();
    open.rebase_freshness_on_open = false;
  }
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  const std::string label = options.get("--label", "period");
  auto period = ledger.value()->open_period(start.value(), end.value(), label);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitFailure;
  }
  std::printf("store=%s\nperiod=%s\n", ledger.value()->stamp().store.to_string().c_str(),
              period.value().to_string().c_str());
  return kExitOk;
}

[[nodiscard]] int command_ingest(const Options& options) {
  if (!options.has("--store") || !options.has("--evidence")) {
    std::fprintf(stderr, "error: ingest requires --store and --evidence\n");
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  std::string text;
  if (options.get("--evidence") == "-") {
    auto read = read_stdin();
    if (!read.has_value()) {
      report_error(read.error());
      return kExitFailure;
    }
    text = read.value();
  } else {
    auto read = read_text_file(options.get("--evidence"));
    if (!read.has_value()) {
      report_error(read.error());
      return kExitFailure;
    }
    text = read.value();
  }
  auto batch = fel::parse_evidence_document(text, options.get("--evidence"));
  if (!batch.has_value()) {
    report_error(batch.error());
    return kExitFailure;
  }
  fel::ClaimOptions claim_options;
  if (options.has("--period")) {
    auto period = required_period(options);
    if (!period.has_value()) {
      report_error(period.error());
      return kExitUsage;
    }
    claim_options.period = period.value();
  }
  auto report = options.has("--parallel") ? ledger.value()->ingest_parallel(batch.value(), claim_options)
                                          : ledger.value()->ingest(batch.value(), claim_options);
  if (!report.has_value()) {
    report_error(report.error());
    return kExitFailure;
  }
  if (options.has("--json")) {
    fel::Json root = fel::Json::object();
    root.set("received", fel::Json::number(report.value().received));
    root.set("accepted", fel::Json::number(report.value().accepted));
    root.set("duplicate_suppressed", fel::Json::number(report.value().duplicate_suppressed));
    root.set("rejected", fel::Json::number(report.value().rejected_shape +
                                           report.value().rejected_unknown_source +
                                           report.value().rejected_retired_incarnation +
                                           report.value().rejected_epoch_fenced +
                                           report.value().rejected_generation +
                                           report.value().rejected_unsupported_provenance +
                                           report.value().rejected_no_period));
    root.set("late_evidence", fel::Json::number(report.value().late_evidence));
    std::printf("%s\n", root.dump(2).c_str());
  } else if (!options.has("--quiet")) {
    std::printf("received=%llu accepted=%llu duplicates=%llu late=%llu\n",
                static_cast<unsigned long long>(report.value().received),
                static_cast<unsigned long long>(report.value().accepted),
                static_cast<unsigned long long>(report.value().duplicate_suppressed),
                static_cast<unsigned long long>(report.value().late_evidence));
  }
  return kExitOk;
}

[[nodiscard]] int command_close(const Options& options, bool correct) {
  if (!options.has("--store")) {
    std::fprintf(stderr, "error: close requires --store\n");
    return kExitUsage;
  }
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  auto when = required_time(options, correct ? "--created-at" : "--closed-at");
  if (!when.has_value()) {
    report_error(when.error());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  const bool dry_run = options.has("--dry-run");
  if (correct) {
    fel::CorrectionRequest request;
    request.period = period.value();
    request.created_at = when.value();
    request.reason = options.get("--reason", "correction");
    request.operator_label = options.get("--operator", "cli");
    request.dry_run = dry_run;
    auto revision = ledger.value()->correct_period(request);
    if (!revision.has_value()) {
      report_error(revision.error());
      return kExitFailure;
    }
    std::printf("revision=%llu digest=%s parent=%s\n",
                static_cast<unsigned long long>(revision.value().revision.value()),
                revision.value().content_digest.to_hex().c_str(),
                revision.value().parent_digest.has_value()
                    ? revision.value().parent_digest->to_hex().c_str()
                    : "-");
    return kExitOk;
  }
  fel::CloseRequest request;
  request.period = period.value();
  request.closed_at = when.value();
  request.reason = options.get("--reason", "close");
  request.operator_label = options.get("--operator", "cli");
  request.dry_run = dry_run;
  auto revision = ledger.value()->close_period(request);
  if (!revision.has_value()) {
    report_error(revision.error());
    return kExitFailure;
  }
  std::printf("revision=%llu claims=%llu digest=%s complete=%s\n",
              static_cast<unsigned long long>(revision.value().revision.value()),
              static_cast<unsigned long long>(revision.value().claims.size()),
              revision.value().content_digest.to_hex().c_str(),
              revision.value().complete ? "true" : "false");
  return kExitOk;
}

[[nodiscard]] int command_query(const Options& options) {
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  fel::QueryRequest request;
  request.period = period.value();
  if (options.has("--revision")) {
    request.revision = fel::RevisionOrdinal{options.get_u64("--revision", 1)};
  }
  if (options.has("--scope")) {
    auto scope = fel::ScopeId::parse(options.get("--scope"));
    if (!scope.has_value()) {
      report_error(scope.error());
      return kExitUsage;
    }
    request.scope = scope.value();
  }
  request.include_descendants = options.has("--descendants");
  if (options.has("--category")) {
    auto category = fel::category_from_name(options.get("--category"));
    if (!category.has_value()) {
      std::fprintf(stderr, "error: unknown category %s\n", options.get("--category").c_str());
      return kExitUsage;
    }
    request.category = category.value();
  }
  if (options.has("--measure")) {
    auto kind = fel::measure_kind_from_name(options.get("--measure"));
    if (!kind.has_value()) {
      std::fprintf(stderr, "error: unknown measure %s\n", options.get("--measure").c_str());
      return kExitUsage;
    }
    request.kind = kind.value();
  }
  if (options.has("--max-rows")) {
    request.max_rows = static_cast<std::size_t>(options.get_u64("--max-rows", 1000));
  }
  auto summary = ledger.value()->query(request);
  if (!summary.has_value()) {
    report_error(summary.error());
    return kExitFailure;
  }
  if (options.has("--json")) {
    std::printf("%s\n", summary.value().to_json().c_str());
    return kExitOk;
  }
  std::printf("period=%s revision=%llu state=%s rows=%llu%s\n",
              summary.value().period.to_string().c_str(),
              static_cast<unsigned long long>(summary.value().revision.value()),
              std::string(fel::period_state_name(summary.value().state)).c_str(),
              static_cast<unsigned long long>(summary.value().rows.size()),
              summary.value().truncation.truncated ? " (truncated)" : "");
  for (const auto& row : summary.value().rows) {
    std::printf("  scope=%s generation=%s measure=%s category=%s coverage=%s total=%llu\n",
                row.scope.is_nil() ? "-" : row.scope.to_string().c_str(),
                row.generation.to_string().c_str(),
                std::string(fel::measure_kind_name(row.kind)).c_str(),
                std::string(fel::category_name(row.category)).c_str(),
                std::string(fel::coverage_name(row.cell.coverage())).c_str(),
                static_cast<unsigned long long>(row.cell.known_total));
  }
  for (const auto& unknown : summary.value().unknowns) {
    std::printf("  unknown reason=%s measure=%s evidence=%llu %s\n",
                std::string(fel::unknown_reason_name(unknown.reason)).c_str(),
                std::string(fel::measure_kind_name(unknown.kind)).c_str(),
                static_cast<unsigned long long>(unknown.evidence_count), unknown.note.c_str());
  }
  for (const auto& summary_entry : summary.value().efficiency) {
    if (summary_entry.denominator == 0 && summary_entry.unknown_contributions == 0) {
      continue;
    }
    std::printf("  efficiency measure=%s coverage=%s ratio=%s %s\n",
                std::string(fel::measure_kind_name(summary_entry.kind)).c_str(),
                std::string(fel::coverage_name(summary_entry.coverage)).c_str(),
                summary_entry.ratio_text().c_str(), summary_entry.indeterminacy_reason.c_str());
  }
  std::printf("proof_surfaces=%s as_of=%s\n", summary.value().stamp.proof_surface_label().c_str(),
              summary.value().stamp.as_of.to_rfc3339_millis().c_str());
  return kExitOk;
}

[[nodiscard]] int command_aggregate(const Options& options) {
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  auto axis = fel::aggregate_axis_from_name(options.get("--axis", "category"));
  if (!axis.has_value()) {
    std::fprintf(stderr, "error: unknown axis %s\n", options.get("--axis").c_str());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  fel::AggregateRequest request;
  request.period = period.value();
  request.axis = axis.value();
  if (options.has("--window-ms")) {
    request.window = fel::Duration::from_millis(
        static_cast<std::int64_t>(options.get_u64("--window-ms", 3600000)));
  }
  auto result = ledger.value()->aggregate(request);
  if (!result.has_value()) {
    report_error(result.error());
    return kExitFailure;
  }
  std::printf("%s\n", result.value().to_json().c_str());
  return kExitOk;
}

[[nodiscard]] int command_reconcile(const Options& options) {
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  fel::ReconcileRequest request;
  request.period = period.value();
  auto report = ledger.value()->reconcile(request);
  if (!report.has_value()) {
    report_error(report.error());
    return kExitFailure;
  }
  std::printf("%s\n", report.value().to_json().c_str());
  return report.value().fully_consistent ? kExitOk : kExitFailure;
}

[[nodiscard]] int command_explain(const Options& options) {
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  if (!options.has("--identity")) {
    std::fprintf(stderr, "error: explain requires --identity\n");
    return kExitUsage;
  }
  auto identity = fel::AccountingIdentity::parse(options.get("--identity"));
  if (!identity.has_value()) {
    report_error(identity.error());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  fel::ExplainRequest request;
  request.period = period.value();
  request.identity = identity.value();
  auto explanation = ledger.value()->explain(request);
  if (!explanation.has_value()) {
    report_error(explanation.error());
    return kExitFailure;
  }
  std::printf("%s\n", explanation.value().to_json().c_str());
  return explanation.value().found ? kExitOk : kExitFailure;
}

[[nodiscard]] int command_export(const Options& options) {
  auto period = required_period(options);
  if (!period.has_value()) {
    report_error(period.error());
    return kExitUsage;
  }
  auto format = fel::export_format_from_name(options.get("--format", "jsonl"));
  if (!format.has_value()) {
    std::fprintf(stderr, "error: unknown format %s\n", options.get("--format").c_str());
    return kExitUsage;
  }
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  fel::ExportRequest request;
  request.period = period.value();
  request.format = format.value();
  auto result = ledger.value()->export_period(request);
  if (!result.has_value()) {
    report_error(result.error());
    return kExitFailure;
  }
  if (options.has("--out")) {
    std::FILE* file = std::fopen(options.get("--out").c_str(), "wb");
    if (file == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", options.get("--out").c_str());
      return kExitFailure;
    }
    std::fwrite(result.value().text.data(), 1, result.value().text.size(), file);
    std::fclose(file);
    if (!options.has("--quiet")) {
      std::printf("rows=%llu digest=%s\n",
                  static_cast<unsigned long long>(result.value().rows),
                  result.value().payload_digest.to_hex().c_str());
    }
    return kExitOk;
  }
  std::fwrite(result.value().text.data(), 1, result.value().text.size(), stdout);
  return kExitOk;
}

[[nodiscard]] int command_verify(const Options& options) {
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  auto report = ledger.value()->verify_integrity();
  if (!report.has_value()) {
    report_error(report.error());
    return kExitFailure;
  }
  std::printf("%s\n", report.value().to_json().c_str());
  return report.value().ok ? kExitOk : kExitFailure;
}

[[nodiscard]] int command_stats(const Options& options) {
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  std::printf("%s\n", ledger.value()->stats().to_json().c_str());
  return kExitOk;
}

[[nodiscard]] int command_compact(const Options& options) {
  auto open = make_open_options(options, true);
  auto ledger = fel::Ledger::open(open);
  if (!ledger.has_value()) {
    report_error(ledger.error());
    return kExitFailure;
  }
  auto report = ledger.value()->compact_closed_periods(options.has("--dry-run"));
  if (!report.has_value()) {
    report_error(report.error());
    return kExitFailure;
  }
  std::printf("performed=%s segments_before=%llu segments_after=%llu records_dropped=%llu\n",
              report.value().performed ? "true" : "false",
              static_cast<unsigned long long>(report.value().segments_before),
              static_cast<unsigned long long>(report.value().segments_after),
              static_cast<unsigned long long>(report.value().records_dropped));
  for (const auto& line : report.value().diagnostics) {
    std::printf("  %s\n", line.c_str());
  }
  return kExitOk;
}

// Waits until the process standard input reaches end of file, or until the
// supplied bound elapses. The bound is what keeps the diagnostic honest.
[[nodiscard]] bool wait_for_stdin_eof(std::uint32_t max_ms) {
#ifdef _WIN32
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const ULONGLONG deadline = GetTickCount64() + max_ms;
  while (true) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    const DWORD remaining = static_cast<DWORD>(deadline - now);
    const DWORD waited = WaitForSingleObject(input, remaining);
    if (waited != WAIT_OBJECT_0) {
      return false;
    }
    char buffer[512];
    DWORD got = 0;
    if (!ReadFile(input, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
      return true;
    }
  }
#else
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd descriptor {};
    descriptor.fd = 0;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, 100);
    if (ready < 0) {
      return false;
    }
    if (ready == 0) {
      continue;
    }
    char buffer[512];
    const ssize_t got = ::read(0, buffer, sizeof(buffer));
    if (got <= 0) {
      return true;
    }
  }
  return false;
#endif
}

[[nodiscard]] int command_lock_hold(const Options& options) {
  if (!options.has("--store")) {
    std::fprintf(stderr, "error: lock-hold requires --store\n");
    return kExitUsage;
  }
  fel::StoreConfig config;
  config.path = options.get("--store");
  config.mode = fel::StoreOpenMode::OpenOrCreate;
  config.lock_timeout_ms = static_cast<std::uint32_t>(options.get_u64("--lock-timeout-ms", 0));
  config.lock_hold_max_ms =
      static_cast<std::uint32_t>(options.get_u64("--lock-hold-max-ms", 30000));
  auto store = fel::LedgerStore::open(config);
  if (!store.has_value()) {
    report_error(store.error());
    return kExitFailure;
  }
  // The marker is printed only after the exclusive lock is genuinely held, so a
  // supervising process can synchronise on it without polling.
  std::fprintf(stderr, "lock-acquired store=%s revision=%llu\n",
               options.get("--store").c_str(),
               static_cast<unsigned long long>(store.value()->revision()));
  std::fflush(stderr);
  if (options.has("--hold-lock-until-eof")) {
    const bool reached_eof = wait_for_stdin_eof(config.lock_hold_max_ms);
    std::fprintf(stderr, "lock-released reason=%s\n", reached_eof ? "eof" : "budget");
  }
  std::fflush(stderr);
  return kExitOk;
}

}  // namespace

int run(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return kExitUsage;
  }
  const std::string command = argv[1];
  Options options;
  if (!parse_options(argc - 2, argv + 2, &options)) {
    std::fprintf(stderr, "error: malformed option list\n");
    return kExitUsage;
  }
  if (command == "version" || command == "--version") {
    std::printf("fel %s (Fabric Efficiency Ledger)\n", std::string(fel::kVersionString).c_str());
    return kExitOk;
  }
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }
  if (command == "init") return command_init(options);
  if (command == "ingest") return command_ingest(options);
  if (command == "close") return command_close(options, false);
  if (command == "correct") return command_close(options, true);
  if (command == "query") return command_query(options);
  if (command == "aggregate") return command_aggregate(options);
  if (command == "reconcile") return command_reconcile(options);
  if (command == "explain") return command_explain(options);
  if (command == "export") return command_export(options);
  if (command == "verify") return command_verify(options);
  if (command == "stats") return command_stats(options);
  if (command == "compact") return command_compact(options);
  if (command == "lock-hold") return command_lock_hold(options);
  std::fprintf(stderr, "error: unknown command %s\n", command.c_str());
  print_usage();
  return kExitUsage;
}

}  // namespace fel_cli