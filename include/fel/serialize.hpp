// Fabric Efficiency Ledger - canonical serialization: JSON, CSV, field writer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/crypto.hpp"
#include "fel/error.hpp"

namespace fel {

// Builds a deterministic, parseable text form and a SHA-256 over the same
// fields with explicit length framing. Text and digest can never disagree
// because both are produced by the same call.
class FieldWriter {
 public:
  FieldWriter() = default;

  void field(std::string_view value);
  void field_u64(std::uint64_t value);
  void field_i64(std::int64_t value);
  void field_bool(bool value);
  void field_bytes(const std::uint8_t* data, std::size_t size);

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  // Idempotent: the digest is finalized once and cached, and any further field
  // invalidates the cache. Calling digest() twice therefore agrees.
  [[nodiscard]] Digest256 digest();
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  void reserve(std::size_t bytes) { text_.reserve(bytes); }

 private:
  std::string text_;
  Sha256 hasher_;
  Digest256 cached_digest_{};
  bool digest_valid_ = false;
};

// A strict, bounded JSON document model. Objects keep keys sorted so that
// serialization is canonical: the same logical document always produces byte
// identical output, which is what makes exported digests comparable.
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  enum class Type : std::uint8_t { Null = 0, Bool = 1, Number = 2, String = 3, Array = 4, Object = 5 };

  Json() = default;

  [[nodiscard]] static Json null();
  [[nodiscard]] static Json boolean(bool value);
  [[nodiscard]] static Json number(std::int64_t value);
  [[nodiscard]] static Json number(std::uint64_t value);
  [[nodiscard]] static Json number(double value);
  [[nodiscard]] static Json string(std::string value);
  [[nodiscard]] static Json array();
  [[nodiscard]] static Json object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

  [[nodiscard]] bool as_bool() const noexcept { return bool_; }
  [[nodiscard]] double as_double() const noexcept { return number_; }
  [[nodiscard]] std::int64_t as_i64() const noexcept { return static_cast<std::int64_t>(number_); }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
  [[nodiscard]] const Array& as_array() const noexcept { return array_; }
  [[nodiscard]] const Object& as_object() const noexcept { return object_; }
  [[nodiscard]] Array& as_array_mut() noexcept { return array_; }
  [[nodiscard]] Object& as_object_mut() noexcept { return object_; }

  // Numeric accessors that refuse to lose information.
  [[nodiscard]] Result<std::uint64_t> to_u64() const;
  [[nodiscard]] Result<std::int64_t> to_i64() const;

  void push(Json value) { array_.push_back(std::move(value)); }
  void set(std::string key, Json value) { object_[std::move(key)] = std::move(value); }

  [[nodiscard]] const Json* find(std::string_view key) const;
  // Object member access that fails loudly instead of returning null.
  [[nodiscard]] Result<const Json*> require(std::string_view key, Type expected) const;
  [[nodiscard]] Result<std::string> require_string(std::string_view key) const;
  [[nodiscard]] Result<std::uint64_t> require_u64(std::string_view key) const;
  [[nodiscard]] Result<bool> require_bool(std::string_view key) const;
  [[nodiscard]] Result<const Json*> require_array(std::string_view key) const;
  [[nodiscard]] Result<const Json*> require_object(std::string_view key) const;

  // Rejects unknown member names: a misspelled policy key must never be ignored.
  [[nodiscard]] Result<void> reject_unknown_keys(
      const std::vector<std::string_view>& allowed) const;

  // Canonical dump: keys sorted, no insignificant whitespace when indent == 0.
  [[nodiscard]] std::string dump(int indent = 0) const;
  [[nodiscard]] std::string dump_canonical() const { return dump(0); }

  // Strict, bounded parse. Rejects duplicate keys, trailing content, comments,
  // NaN/Infinity and any document deeper than limits::kMaxJsonDepth.
  [[nodiscard]] static Result<Json> parse(std::string_view text);

 private:
  void dump_into(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::int64_t integer_ = 0;
  std::uint64_t unsigned_integer_ = 0;
  bool is_integer_ = false;
  bool is_unsigned_ = false;
  std::string string_;
  Array array_;
  Object object_;
};

// RFC 4180 style CSV with deterministic quoting.
class CsvWriter {
 public:
  explicit CsvWriter(std::string_view header, char delimiter = ',');

  void row(const std::vector<std::string>& fields);
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }

  [[nodiscard]] static std::string escape_field(std::string_view value, char delimiter);

 private:
  std::string text_;
  char delimiter_ = ',';
  std::uint64_t rows_ = 0;
};

// Escapes a value so it can be embedded in a single line field format.
[[nodiscard]] std::string escape_inline(std::string_view value);

}  // namespace fel
