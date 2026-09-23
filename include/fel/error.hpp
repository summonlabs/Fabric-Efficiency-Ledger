// Fabric Efficiency Ledger - error taxonomy and result type.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace fel {

// Every failure surface of the runtime is enumerated. The taxonomy is
// deliberately explicit: callers must be able to distinguish "the operator
// asked for something invalid" from "the telemetry disagrees with itself"
// from "the runtime is out of budget".
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // --- Input shape / decoding -------------------------------------------------
  InvalidArgument = 100,
  MalformedJson = 101,
  MalformedTimestamp = 102,
  MalformedIdentity = 103,
  SchemaViolation = 104,
  UnknownField = 105,
  MissingRequiredField = 106,
  UnsupportedFormatVersion = 107,
  PayloadTooLarge = 108,
  MetadataTooLarge = 109,
  TooManyItems = 110,
  DuplicateDefinition = 111,

  // --- Domain validation ------------------------------------------------------
  UnknownSource = 200,
  UnknownResource = 201,
  UnknownScope = 202,
  UnknownGeneration = 203,
  UnknownIncarnation = 204,
  UnknownPeriod = 205,
  UnknownMeasureKind = 206,
  UnknownCategory = 207,
  IncompatibleMeasureKind = 208,
  ScopeCycleDetected = 209,
  ScopeMultipleParents = 210,
  ResourceInMultipleScopes = 211,
  OverlappingIncarnations = 212,
  RetiredIncarnation = 213,
  GenerationMismatch = 214,
  TopologyRevisionMismatch = 215,
  PolicyRevisionMismatch = 216,

  // --- Evidence integrity / fencing -------------------------------------------
  EpochRegression = 300,
  SequenceReplay = 301,
  DuplicateEvidence = 302,
  ConflictingEvidence = 303,
  EvidenceChecksumMismatch = 304,
  ClockRegression = 305,
  FutureEvidence = 306,
  StaleEvidence = 307,
  ExpiredEvidence = 308,
  UnsupportedProvenance = 309,
  NoEvidence = 310,

  // --- Accounting -------------------------------------------------------------
  OverAttribution = 400,
  ConservationViolation = 401,
  MixedGenerationConservation = 402,
  UnknownBucketNotEmpty = 403,
  AttributionFailure = 404,
  ResidualNotAttributable = 405,

  // --- Lifecycle --------------------------------------------------------------
  PeriodAlreadyClosed = 500,
  PeriodNotClosed = 501,
  PeriodNotOpen = 502,
  CorrectionChainFork = 503,
  CorrectionLimitReached = 504,
  LateEvidenceRejected = 505,
  ImmutableRecordMutation = 506,
  NoSuchRevision = 507,
  CorrectionRequired = 508,

  // --- Persistence ------------------------------------------------------------
  StoreNotFound = 600,
  StoreCorrupt = 601,
  StoreFormatMismatch = 602,
  ManifestMissing = 603,
  ManifestCorrupt = 604,
  SegmentCorrupt = 605,
  SegmentMissing = 606,
  RecordChecksumMismatch = 607,
  StoreLocked = 608,
  StoreReadOnly = 609,
  IoFailure = 610,
  RecoveryFailed = 611,
  CapacityExceeded = 612,

  // --- Runtime / concurrency --------------------------------------------------
  QueueClosed = 700,
  QueueFull = 701,
  Cancelled = 702,
  WorkerFailure = 703,
  ReentrantLock = 704,
  LockOrderViolation = 705,
  ShutdownInProgress = 706,
  ArithmeticOverflow = 707,

  // --- Query surface ----------------------------------------------------------
  ResultSetTruncated = 800,
  UnsupportedQuery = 801,

  Internal = 900,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

// Stable, machine readable category used by callers that must decide whether a
// failure is retryable, fatal, or the caller's fault.
[[nodiscard]] std::string_view error_code_domain(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  std::string detail;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg, std::string det)
      : code(c), message(std::move(msg)), detail(std::move(det)) {}

  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message), {}};
}

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message, std::string detail) {
  return Error{code, std::move(message), std::move(detail)};
}

// A minimal, dependency free Result. std::expected is C++23 and this project
// targets C++20 with a strict, vendor neutral toolchain policy.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}      // NOLINT
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }

  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }

  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const {
    return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

  template <class F>
  [[nodiscard]] auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>> {
    using Out = std::invoke_result_t<F, const T&>;
    if (!has_value()) {
      return Result<Out>(error());
    }
    return Result<Out>(std::forward<F>(fn)(value()));
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  using value_type = void;

  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return error_.code == ErrorCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  void value() const noexcept {}

  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

[[nodiscard]] inline Result<void> ok() { return Result<void>{}; }

}  // namespace fel

// Propagates the failure of an expression yielding a fel::Result out of the
// enclosing function, which must itself return a fel::Result. Both forms expand
// to plain statements in the enclosing scope, so FEL_TRY_ASSIGN can introduce a
// new binding that stays visible to the statements that follow it.
#define FEL_DETAIL_CONCAT_INNER(a, b) a##b
#define FEL_DETAIL_CONCAT(a, b) FEL_DETAIL_CONCAT_INNER(a, b)

#define FEL_TRY(expr)                                                  \
  auto FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__) = (expr);           \
  if (!FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__).has_value()) {      \
    return FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__).error();        \
  }                                                                    \
  static_assert(true, "FEL_TRY requires a trailing semicolon")

#define FEL_TRY_ASSIGN(dest, expr)                                     \
  auto FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__) = (expr);           \
  if (!FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__).has_value()) {      \
    return FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__).error();        \
  }                                                                    \
  dest = std::move(FEL_DETAIL_CONCAT(fel_try_probe_, __LINE__)).value(); \
  static_assert(true, "FEL_TRY_ASSIGN requires a trailing semicolon")
