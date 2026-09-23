// Fabric Efficiency Ledger - error taxonomy names, domains and rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fel/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fel {

std::string_view error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "Ok";

    case ErrorCode::InvalidArgument:
      return "InvalidArgument";
    case ErrorCode::MalformedJson:
      return "MalformedJson";
    case ErrorCode::MalformedTimestamp:
      return "MalformedTimestamp";
    case ErrorCode::MalformedIdentity:
      return "MalformedIdentity";
    case ErrorCode::SchemaViolation:
      return "SchemaViolation";
    case ErrorCode::UnknownField:
      return "UnknownField";
    case ErrorCode::MissingRequiredField:
      return "MissingRequiredField";
    case ErrorCode::UnsupportedFormatVersion:
      return "UnsupportedFormatVersion";
    case ErrorCode::PayloadTooLarge:
      return "PayloadTooLarge";
    case ErrorCode::MetadataTooLarge:
      return "MetadataTooLarge";
    case ErrorCode::TooManyItems:
      return "TooManyItems";
    case ErrorCode::DuplicateDefinition:
      return "DuplicateDefinition";

    case ErrorCode::UnknownSource:
      return "UnknownSource";
    case ErrorCode::UnknownResource:
      return "UnknownResource";
    case ErrorCode::UnknownScope:
      return "UnknownScope";
    case ErrorCode::UnknownGeneration:
      return "UnknownGeneration";
    case ErrorCode::UnknownIncarnation:
      return "UnknownIncarnation";
    case ErrorCode::UnknownPeriod:
      return "UnknownPeriod";
    case ErrorCode::UnknownMeasureKind:
      return "UnknownMeasureKind";
    case ErrorCode::UnknownCategory:
      return "UnknownCategory";
    case ErrorCode::IncompatibleMeasureKind:
      return "IncompatibleMeasureKind";
    case ErrorCode::ScopeCycleDetected:
      return "ScopeCycleDetected";
    case ErrorCode::ScopeMultipleParents:
      return "ScopeMultipleParents";
    case ErrorCode::ResourceInMultipleScopes:
      return "ResourceInMultipleScopes";
    case ErrorCode::OverlappingIncarnations:
      return "OverlappingIncarnations";
    case ErrorCode::RetiredIncarnation:
      return "RetiredIncarnation";
    case ErrorCode::GenerationMismatch:
      return "GenerationMismatch";
    case ErrorCode::TopologyRevisionMismatch:
      return "TopologyRevisionMismatch";
    case ErrorCode::PolicyRevisionMismatch:
      return "PolicyRevisionMismatch";

    case ErrorCode::EpochRegression:
      return "EpochRegression";
    case ErrorCode::SequenceReplay:
      return "SequenceReplay";
    case ErrorCode::DuplicateEvidence:
      return "DuplicateEvidence";
    case ErrorCode::ConflictingEvidence:
      return "ConflictingEvidence";
    case ErrorCode::EvidenceChecksumMismatch:
      return "EvidenceChecksumMismatch";
    case ErrorCode::ClockRegression:
      return "ClockRegression";
    case ErrorCode::FutureEvidence:
      return "FutureEvidence";
    case ErrorCode::StaleEvidence:
      return "StaleEvidence";
    case ErrorCode::ExpiredEvidence:
      return "ExpiredEvidence";
    case ErrorCode::UnsupportedProvenance:
      return "UnsupportedProvenance";
    case ErrorCode::NoEvidence:
      return "NoEvidence";

    case ErrorCode::OverAttribution:
      return "OverAttribution";
    case ErrorCode::ConservationViolation:
      return "ConservationViolation";
    case ErrorCode::MixedGenerationConservation:
      return "MixedGenerationConservation";
    case ErrorCode::UnknownBucketNotEmpty:
      return "UnknownBucketNotEmpty";
    case ErrorCode::AttributionFailure:
      return "AttributionFailure";
    case ErrorCode::ResidualNotAttributable:
      return "ResidualNotAttributable";

    case ErrorCode::PeriodAlreadyClosed:
      return "PeriodAlreadyClosed";
    case ErrorCode::PeriodNotClosed:
      return "PeriodNotClosed";
    case ErrorCode::PeriodNotOpen:
      return "PeriodNotOpen";
    case ErrorCode::CorrectionChainFork:
      return "CorrectionChainFork";
    case ErrorCode::CorrectionLimitReached:
      return "CorrectionLimitReached";
    case ErrorCode::LateEvidenceRejected:
      return "LateEvidenceRejected";
    case ErrorCode::ImmutableRecordMutation:
      return "ImmutableRecordMutation";
    case ErrorCode::NoSuchRevision:
      return "NoSuchRevision";
    case ErrorCode::CorrectionRequired:
      return "CorrectionRequired";

    case ErrorCode::StoreNotFound:
      return "StoreNotFound";
    case ErrorCode::StoreCorrupt:
      return "StoreCorrupt";
    case ErrorCode::StoreFormatMismatch:
      return "StoreFormatMismatch";
    case ErrorCode::ManifestMissing:
      return "ManifestMissing";
    case ErrorCode::ManifestCorrupt:
      return "ManifestCorrupt";
    case ErrorCode::SegmentCorrupt:
      return "SegmentCorrupt";
    case ErrorCode::SegmentMissing:
      return "SegmentMissing";
    case ErrorCode::RecordChecksumMismatch:
      return "RecordChecksumMismatch";
    case ErrorCode::StoreLocked:
      return "StoreLocked";
    case ErrorCode::StoreReadOnly:
      return "StoreReadOnly";
    case ErrorCode::IoFailure:
      return "IoFailure";
    case ErrorCode::RecoveryFailed:
      return "RecoveryFailed";
    case ErrorCode::CapacityExceeded:
      return "CapacityExceeded";

    case ErrorCode::QueueClosed:
      return "QueueClosed";
    case ErrorCode::QueueFull:
      return "QueueFull";
    case ErrorCode::Cancelled:
      return "Cancelled";
    case ErrorCode::WorkerFailure:
      return "WorkerFailure";
    case ErrorCode::ReentrantLock:
      return "ReentrantLock";
    case ErrorCode::LockOrderViolation:
      return "LockOrderViolation";
    case ErrorCode::ShutdownInProgress:
      return "ShutdownInProgress";
    case ErrorCode::ArithmeticOverflow:
      return "ArithmeticOverflow";

    case ErrorCode::ResultSetTruncated:
      return "ResultSetTruncated";
    case ErrorCode::UnsupportedQuery:
      return "UnsupportedQuery";

    case ErrorCode::Internal:
      return "Internal";
  }
  return "Unknown";
}

std::string_view error_code_domain(ErrorCode code) noexcept {
  // Domains mirror the taxonomy blocks of fel/error.hpp exactly: each block is a
  // contiguous numeric range, and the returned strings are part of the stable
  // machine readable surface (callers use them to decide retry versus abort).
  const auto value = static_cast<std::uint16_t>(code);
  if (value == static_cast<std::uint16_t>(ErrorCode::Ok)) {
    return "ok";
  }
  if (value >= 100 && value < 200) {
    return "input";
  }
  if (value >= 200 && value < 300) {
    return "domain";
  }
  if (value >= 300 && value < 400) {
    return "integrity";
  }
  if (value >= 400 && value < 500) {
    return "accounting";
  }
  if (value >= 500 && value < 600) {
    return "lifecycle";
  }
  if (value >= 600 && value < 700) {
    return "persistence";
  }
  if (value >= 700 && value < 800) {
    return "runtime";
  }
  if (value >= 800 && value < 900) {
    return "query";
  }
  if (value >= 900 && value < 1000) {
    return "internal";
  }
  return "unknown";
}

std::string Error::to_string() const {
  std::string out;
  out.reserve(message.size() + detail.size() + 48);
  out.append(error_code_name(code));
  out.push_back('(');
  out.append(std::to_string(static_cast<std::uint16_t>(code)));
  out.push_back(')');
  if (!message.empty()) {
    out.append(": ");
    out.append(message);
  }
  if (!detail.empty()) {
    out.append("; detail: ");
    out.append(detail);
  }
  return out;
}

}  // namespace fel
