// Fabric Efficiency Ledger - canonical serialization: JSON, CSV, field writer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/serialize.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "fel/limits.hpp"

namespace fel {
namespace {

constexpr char kFieldSeparator = '\x1f';

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool is_hex_digit(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7FU) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

void escape_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char raw : value) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20U) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

// ---------------------------------------------------------------------------
// Strict JSON parser
// ---------------------------------------------------------------------------
class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  [[nodiscard]] Result<Json> run() {
    skip_whitespace();
    Json value;
    FEL_TRY_ASSIGN(value, parse_value(0));
    skip_whitespace();
    if (pos_ != text_.size()) {
      return failure("trailing content after JSON document");
    }
    return value;
  }

 private:
  [[nodiscard]] Error failure(std::string message) const {
    return make_error(ErrorCode::MalformedJson, std::move(message),
                      "offset " + std::to_string(pos_));
  }

  [[nodiscard]] bool consume(char expected) noexcept {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  void skip_whitespace() noexcept {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] Result<bool> literal(std::string_view word) {
    if (text_.size() - pos_ < word.size()) {
      return failure("truncated literal");
    }
    if (text_.compare(pos_, word.size(), word) != 0) {
      return failure("invalid literal");
    }
    pos_ += word.size();
    return true;
  }

  [[nodiscard]] Result<std::string> parse_string() {
    if (!consume('"')) {
      return failure("expected string");
    }
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) {
        return failure("unterminated string");
      }
      const char c = text_[pos_++];
      if (c == '"') {
        break;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        return failure("raw control character in string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return failure("truncated escape sequence");
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (text_.size() - pos_ < 4) {
            return failure("truncated unicode escape");
          }
          std::uint32_t code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_ + static_cast<std::size_t>(i)];
            if (!is_hex_digit(h)) {
              return failure("invalid unicode escape");
            }
            code = (code << 4) | static_cast<std::uint32_t>(hex_value(h));
          }
          pos_ += 4;
          if (code >= 0xD800U && code <= 0xDBFFU) {
            if (text_.size() - pos_ < 6 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              return failure("unpaired high surrogate");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = text_[pos_ + static_cast<std::size_t>(i)];
              if (!is_hex_digit(h)) {
                return failure("invalid low surrogate");
              }
              low = (low << 4) | static_cast<std::uint32_t>(hex_value(h));
            }
            pos_ += 4;
            if (low < 0xDC00U || low > 0xDFFFU) {
              return failure("invalid low surrogate");
            }
            code = 0x10000U + ((code - 0xD800U) << 10) + (low - 0xDC00U);
          } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            return failure("unpaired low surrogate");
          }
          append_utf8(out, code);
          break;
        }
        default:
          return failure("unknown escape sequence");
      }
      if (out.size() > limits::kMaxRecordBytes) {
        return make_error(ErrorCode::PayloadTooLarge, "JSON string exceeds the payload budget");
      }
    }
    return out;
  }

  [[nodiscard]] Result<Json> parse_number() {
    const std::size_t start = pos_;
    bool is_integer = true;
    if (consume('-')) {
      // sign handled below by re-scan
    }
    if (pos_ >= text_.size()) {
      return failure("truncated number");
    }
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (is_digit(text_[pos_])) {
      while (pos_ < text_.size() && is_digit(text_[pos_])) {
        ++pos_;
      }
    } else {
      return failure("invalid number");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_integer = false;
      ++pos_;
      if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
        return failure("missing fraction digits");
      }
      while (pos_ < text_.size() && is_digit(text_[pos_])) {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_integer = false;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
        return failure("missing exponent digits");
      }
      while (pos_ < text_.size() && is_digit(text_[pos_])) {
        ++pos_;
      }
    }
    const std::string literal(text_.substr(start, pos_ - start));
    if (is_integer) {
      std::size_t consumed = 0;
      try {
        const long long value = std::stoll(literal, &consumed);
        if (consumed == literal.size()) {
          return Json::number(static_cast<std::int64_t>(value));
        }
      } catch (const std::exception&) {
        // Falls through to the unsigned representation below.
      }
      if (!literal.empty() && literal.front() != '-') {
        std::size_t unsigned_consumed = 0;
        try {
          const unsigned long long value = std::stoull(literal, &unsigned_consumed);
          if (unsigned_consumed == literal.size()) {
            return Json::number(static_cast<std::uint64_t>(value));
          }
        } catch (const std::exception&) {
          // Falls through to the double representation for out-of-range integers.
        }
      }
    }
    char* end = nullptr;
    const double value = std::strtod(literal.c_str(), &end);
    if (end == nullptr || *end != '\0' || std::isinf(value) || std::isnan(value)) {
      return make_error(ErrorCode::MalformedJson, "number out of range", literal);
    }
    return Json::number(value);
  }

  [[nodiscard]] Result<Json> parse_value(std::size_t depth) {
    if (depth > limits::kMaxJsonDepth) {
      return make_error(ErrorCode::MalformedJson, "JSON document exceeds the depth budget");
    }
    if (++tokens_ > limits::kMaxJsonTokens) {
      return make_error(ErrorCode::TooManyItems, "JSON document exceeds the token budget");
    }
    skip_whitespace();
    if (pos_ >= text_.size()) {
      return failure("unexpected end of document");
    }
    const char c = text_[pos_];
    if (c == '{') {
      ++pos_;
      Json object = Json::object();
      skip_whitespace();
      if (consume('}')) {
        return object;
      }
      while (true) {
        skip_whitespace();
        std::string key;
        FEL_TRY_ASSIGN(key, parse_string());
        skip_whitespace();
        if (!consume(':')) {
          return failure("expected ':' after object key");
        }
        skip_whitespace();
        Json member;
        FEL_TRY_ASSIGN(member, parse_value(depth + 1));
        if (object.as_object().find(key) != object.as_object().end()) {
          return make_error(ErrorCode::MalformedJson, "duplicate object key", key);
        }
        object.set(std::move(key), std::move(member));
        skip_whitespace();
        if (consume(',')) {
          continue;
        }
        if (consume('}')) {
          return object;
        }
        return failure("expected ',' or '}' in object");
      }
    }
    if (c == '[') {
      ++pos_;
      Json array = Json::array();
      skip_whitespace();
      if (consume(']')) {
        return array;
      }
      while (true) {
        skip_whitespace();
        Json element;
        FEL_TRY_ASSIGN(element, parse_value(depth + 1));
        array.push(std::move(element));
        if (array.as_array().size() > limits::kMaxBatchRecords) {
          return make_error(ErrorCode::TooManyItems, "JSON array exceeds the element budget");
        }
        skip_whitespace();
        if (consume(',')) {
          continue;
        }
        if (consume(']')) {
          return array;
        }
        return failure("expected ',' or ']' in array");
      }
    }
    if (c == '"') {
      std::string text;
      FEL_TRY_ASSIGN(text, parse_string());
      return Json::string(std::move(text));
    }
    if (c == 't') {
      FEL_TRY(literal("true"));
      return Json::boolean(true);
    }
    if (c == 'f') {
      FEL_TRY(literal("false"));
      return Json::boolean(false);
    }
    if (c == 'n') {
      FEL_TRY(literal("null"));
      return Json::null();
    }
    if (c == '-' || is_digit(c)) {
      return parse_number();
    }
    return failure("unexpected character");
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t tokens_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// FieldWriter
// ---------------------------------------------------------------------------
void FieldWriter::field(std::string_view value) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  for (const char raw : value) {
    const unsigned char c = static_cast<unsigned char>(raw);
    // Everything that could be confused with framing is escaped as a backslash
    // followed by two lowercase hex digits, which keeps the text unambiguous
    // and reversible.
    if (c == kFieldSeparator || c == '\\' || c == '\n' || c == '\r') {
      text_.push_back('\\');
      text_.push_back(kHexDigits[(c >> 4) & 0x0FU]);
      text_.push_back(kHexDigits[c & 0x0FU]);
    } else {
      text_.push_back(raw);
    }
  }
  text_.push_back(kFieldSeparator);
  hasher_.update_field(value);
  digest_valid_ = false;
}

Digest256 FieldWriter::digest() {
  if (!digest_valid_) {
    cached_digest_ = hasher_.finish();
    digest_valid_ = true;
  }
  return cached_digest_;
}

void FieldWriter::field_u64(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  field(buffer);
}

void FieldWriter::field_i64(std::int64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  field(buffer);
}

void FieldWriter::field_bool(bool value) { field(value ? "true" : "false"); }

void FieldWriter::field_bytes(const std::uint8_t* data, std::size_t size) {
  field(to_hex(data, size));
}

// ---------------------------------------------------------------------------
// Json
// ---------------------------------------------------------------------------
Json Json::null() { return Json{}; }

Json Json::boolean(bool value) {
  Json json;
  json.type_ = Type::Bool;
  json.bool_ = value;
  return json;
}

Json Json::number(std::int64_t value) {
  Json json;
  json.type_ = Type::Number;
  json.number_ = static_cast<double>(value);
  json.integer_ = value;
  json.is_integer_ = true;
  json.is_unsigned_ = false;
  return json;
}

Json Json::number(std::uint64_t value) {
  Json json;
  json.type_ = Type::Number;
  json.number_ = static_cast<double>(value);
  json.unsigned_integer_ = value;
  json.integer_ = value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                      ? static_cast<std::int64_t>(value)
                      : 0;
  json.is_integer_ = true;
  json.is_unsigned_ = true;
  return json;
}

Json Json::number(double value) {
  Json json;
  json.type_ = Type::Number;
  json.number_ = value;
  json.is_integer_ = false;
  return json;
}

Json Json::string(std::string value) {
  Json json;
  json.type_ = Type::String;
  json.string_ = std::move(value);
  return json;
}

Json Json::array() {
  Json json;
  json.type_ = Type::Array;
  return json;
}

Json Json::object() {
  Json json;
  json.type_ = Type::Object;
  return json;
}

Result<std::uint64_t> Json::to_u64() const {
  if (type_ != Type::Number) {
    return make_error(ErrorCode::SchemaViolation, "expected a number");
  }
  if (is_integer_) {
    if (is_unsigned_) {
      return unsigned_integer_;
    }
    if (integer_ < 0) {
      return make_error(ErrorCode::SchemaViolation, "expected a non negative integer");
    }
    return static_cast<std::uint64_t>(integer_);
  }
  if (number_ < 0.0 || std::floor(number_) != number_ ||
      number_ > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    return make_error(ErrorCode::SchemaViolation, "expected a non negative integer");
  }
  return static_cast<std::uint64_t>(number_);
}

Result<std::int64_t> Json::to_i64() const {
  if (type_ != Type::Number) {
    return make_error(ErrorCode::SchemaViolation, "expected a number");
  }
  if (is_integer_) {
    if (is_unsigned_) {
      if (unsigned_integer_ >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return make_error(ErrorCode::SchemaViolation,
                          "integer does not fit in a signed 64 bit value");
      }
      return static_cast<std::int64_t>(unsigned_integer_);
    }
    return integer_;
  }
  if (std::floor(number_) != number_ ||
      number_ < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      number_ > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return make_error(ErrorCode::SchemaViolation, "expected an integer");
  }
  return static_cast<std::int64_t>(number_);
}

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::Object) {
    return nullptr;
  }
  const auto it = object_.find(std::string(key));
  if (it == object_.end()) {
    return nullptr;
  }
  return &it->second;
}

Result<const Json*> Json::require(std::string_view key, Type expected) const {
  if (type_ != Type::Object) {
    return make_error(ErrorCode::SchemaViolation, "expected an object");
  }
  const Json* member = find(key);
  if (member == nullptr) {
    return make_error(ErrorCode::MissingRequiredField, "missing required field",
                      std::string(key));
  }
  if (member->type() != expected) {
    return make_error(ErrorCode::SchemaViolation, "field has the wrong JSON type",
                      std::string(key));
  }
  return member;
}

Result<std::string> Json::require_string(std::string_view key) const {
  const Json* member = find(key);
  if (member == nullptr) {
    return make_error(ErrorCode::MissingRequiredField, "missing required field",
                      std::string(key));
  }
  if (!member->is_string()) {
    return make_error(ErrorCode::SchemaViolation, "field is not a string", std::string(key));
  }
  return member->as_string();
}

Result<std::uint64_t> Json::require_u64(std::string_view key) const {
  const Json* member = find(key);
  if (member == nullptr) {
    return make_error(ErrorCode::MissingRequiredField, "missing required field",
                      std::string(key));
  }
  return member->to_u64();
}

Result<bool> Json::require_bool(std::string_view key) const {
  const Json* member = find(key);
  if (member == nullptr) {
    return make_error(ErrorCode::MissingRequiredField, "missing required field",
                      std::string(key));
  }
  if (!member->is_bool()) {
    return make_error(ErrorCode::SchemaViolation, "field is not a boolean", std::string(key));
  }
  return member->as_bool();
}

Result<const Json*> Json::require_array(std::string_view key) const {
  return require(key, Type::Array);
}

Result<const Json*> Json::require_object(std::string_view key) const {
  return require(key, Type::Object);
}

Result<void> Json::reject_unknown_keys(const std::vector<std::string_view>& allowed) const {
  if (type_ != Type::Object) {
    return make_error(ErrorCode::SchemaViolation, "expected an object");
  }
  for (const auto& entry : object_) {
    bool known = false;
    for (const auto candidate : allowed) {
      if (entry.first == candidate) {
        known = true;
        break;
      }
    }
    if (!known) {
      return make_error(ErrorCode::UnknownField, "unknown field in document", entry.first);
    }
  }
  return ok();
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  return out;
}

void Json::dump_into(std::string& out, int indent, int depth) const {
  switch (type_) {
    case Type::Null:
      out += "null";
      return;
    case Type::Bool:
      out += bool_ ? "true" : "false";
      return;
    case Type::Number: {
      if (is_integer_) {
        out += is_unsigned_ ? std::to_string(unsigned_integer_) : std::to_string(integer_);
        return;
      }
      char buffer[40];
      std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
      out += buffer;
      return;
    }
    case Type::String:
      escape_json_string(out, string_);
      return;
    case Type::Array: {
      if (array_.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const auto& element : array_) {
        if (!first) {
          out.push_back(',');
        }
        if (indent > 0) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent * (depth + 1)), ' ');
        }
        first = false;
        element.dump_into(out, indent, depth + 1);
      }
      if (indent > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * depth), ' ');
      }
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (object_.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& entry : object_) {
        if (!first) {
          out.push_back(',');
        }
        if (indent > 0) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent * (depth + 1)), ' ');
        }
        first = false;
        escape_json_string(out, entry.first);
        out.push_back(':');
        if (indent > 0) {
          out.push_back(' ');
        }
        entry.second.dump_into(out, indent, depth + 1);
      }
      if (indent > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * depth), ' ');
      }
      out.push_back('}');
      return;
    }
  }
}

Result<Json> Json::parse(std::string_view text) {
  if (text.size() > limits::kMaxRecordBytes * 8U) {
    return make_error(ErrorCode::PayloadTooLarge, "JSON document exceeds the payload budget");
  }
  JsonParser parser(text);
  return parser.run();
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------
CsvWriter::CsvWriter(std::string_view header, char delimiter) : delimiter_(delimiter) {
  text_ += header;
  text_ += "\r\n";
}

std::string CsvWriter::escape_field(std::string_view value, char delimiter) {
  bool needs_quotes = false;
  for (const char c : value) {
    if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return std::string(value);
  }
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char c : value) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

void CsvWriter::row(const std::vector<std::string>& fields) {
  bool first = true;
  for (const auto& field_value : fields) {
    if (!first) {
      text_.push_back(delimiter_);
    }
    first = false;
    text_ += escape_field(field_value, delimiter_);
  }
  text_ += "\r\n";
  ++rows_;
}

std::string escape_inline(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace fel
