// Fabric Efficiency Ledger - canonical JSON, CSV and field framing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <limits>
#include <string>

#include "fel/limits.hpp"
#include "fel/serialize.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, json_round_trip_preserves_structure) {
  const std::string text =
      "{\"b\":[1,2,3],\"a\":{\"nested\":true},\"c\":\"text with \\\"quotes\\\" and \\n newline\","
      "\"d\":null,\"e\":-17}";
  auto parsed = Json::parse(text);
  FEL_REQUIRE(parsed.has_value());
  const std::string dumped = parsed.value().dump(0);
  auto reparsed = Json::parse(dumped);
  FEL_REQUIRE(reparsed.has_value());
  FEL_EXPECT_EQ(reparsed.value().dump(0), dumped);
}

FEL_TEST(unit, json_object_keys_are_written_in_sorted_order) {
  Json object = Json::object();
  object.set("zulu", Json::number(static_cast<std::uint64_t>(1)));
  object.set("alpha", Json::number(static_cast<std::uint64_t>(2)));
  object.set("mike", Json::number(static_cast<std::uint64_t>(3)));
  const std::string dumped = object.dump(0);
  const std::size_t alpha = dumped.find("alpha");
  const std::size_t mike = dumped.find("mike");
  const std::size_t zulu = dumped.find("zulu");
  FEL_REQUIRE(alpha != std::string::npos);
  FEL_REQUIRE(mike != std::string::npos);
  FEL_REQUIRE(zulu != std::string::npos);
  FEL_EXPECT(alpha < mike);
  FEL_EXPECT(mike < zulu);
}

FEL_TEST(unit, json_rejects_malformed_documents) {
  FEL_EXPECT_EQ(Json::parse("{\"a\":1} trailing").error().code, ErrorCode::MalformedJson);
  FEL_EXPECT_EQ(Json::parse("{\"a\":1,\"a\":2}").error().code, ErrorCode::MalformedJson);
  FEL_EXPECT_EQ(Json::parse("\"unterminated").error().code, ErrorCode::MalformedJson);
  FEL_EXPECT_EQ(Json::parse("[1,2,").error().code, ErrorCode::MalformedJson);
  FEL_EXPECT_EQ(Json::parse("{").error().code, ErrorCode::MalformedJson);

  std::string deep;
  for (std::size_t i = 0; i < limits::kMaxJsonDepth + 4; ++i) {
    deep.push_back('[');
  }
  FEL_EXPECT(!Json::parse(deep).has_value());

  const std::string control = std::string("{\"a\":\"") + '\x01' + "\"}";
  FEL_EXPECT_EQ(Json::parse(control).error().code, ErrorCode::MalformedJson);
}

FEL_TEST(unit, json_typed_accessors_report_specific_errors) {
  auto parsed = Json::parse("{\"s\":\"text\",\"n\":42,\"b\":true,\"a\":[1],\"o\":{}}");
  FEL_REQUIRE(parsed.has_value());
  const Json& document = parsed.value();
  FEL_EXPECT_EQ(document.require_string("s").value(), std::string("text"));
  FEL_EXPECT_EQ(document.require_u64("n").value(), std::uint64_t{42});
  FEL_EXPECT(document.require_bool("b").value());
  FEL_EXPECT(document.require_array("a").has_value());
  FEL_EXPECT(document.require_object("o").has_value());
  FEL_EXPECT_EQ(document.require_string("missing").error().code, ErrorCode::MissingRequiredField);
  FEL_EXPECT_EQ(document.require_string("n").error().code, ErrorCode::SchemaViolation);
  FEL_EXPECT_EQ(document.require_u64("s").error().code, ErrorCode::SchemaViolation);
  FEL_EXPECT_EQ(document.require_bool("s").error().code, ErrorCode::SchemaViolation);
}

FEL_TEST(unit, json_unknown_key_rejection) {
  auto parsed = Json::parse("{\"known\":1,\"typo\":2}");
  FEL_REQUIRE(parsed.has_value());
  FEL_ASSERT_OK(parsed.value().reject_unknown_keys({"known", "typo"}));
  FEL_ASSERT_ERR(parsed.value().reject_unknown_keys({"known"}), ErrorCode::UnknownField);
}

FEL_TEST(unit, json_negative_and_unsigned_numbers) {
  auto negative = Json::parse("-5");
  FEL_REQUIRE(negative.has_value());
  FEL_EXPECT_EQ(negative.value().to_i64().value(), std::int64_t{-5});
  FEL_EXPECT_EQ(negative.value().to_u64().error().code, ErrorCode::SchemaViolation);

  auto big = Json::parse("18446744073709551615");
  FEL_REQUIRE(big.has_value());
  FEL_EXPECT_EQ(big.value().to_u64().value(), std::numeric_limits<std::uint64_t>::max());
}

FEL_TEST(unit, csv_quoting_follows_rfc4180) {
  FEL_EXPECT_EQ(CsvWriter::escape_field("plain", ','), std::string("plain"));
  FEL_EXPECT_EQ(CsvWriter::escape_field("a,b", ','), std::string("\"a,b\""));
  FEL_EXPECT_EQ(CsvWriter::escape_field("say \"hi\"", ','), std::string("\"say \"\"hi\"\"\""));
  FEL_EXPECT_EQ(CsvWriter::escape_field("line\nbreak", ','), std::string("\"line\nbreak\""));

  CsvWriter writer("a,b");
  writer.row({"1", "x,y"});
  FEL_EXPECT_EQ(writer.rows(), std::uint64_t{1});
  FEL_EXPECT(writer.text().find("\"x,y\"") != std::string::npos);
}

FEL_TEST(unit, field_writer_framing_distinguishes_field_splits) {
  FieldWriter first;
  first.field("ab");
  first.field("c");
  FieldWriter second;
  second.field("a");
  second.field("bc");
  FEL_EXPECT(first.digest() != second.digest());

  FieldWriter repeated;
  repeated.field("ab");
  repeated.field("c");
  FEL_EXPECT_EQ(repeated.digest().to_hex(), first.digest().to_hex());
  FEL_EXPECT_EQ(repeated.text(), first.text());
}

FEL_TEST(unit, field_writer_escapes_separators_in_text) {
  FieldWriter writer;
  writer.field(std::string("a") + '\x1f' + "b");
  writer.field_u64(7);
  writer.field_bool(true);
  FEL_EXPECT(writer.text().find('\x1f') == writer.text().size() - 1 ||
             writer.text().find("\\1f") != std::string::npos);
  FEL_EXPECT_EQ(escape_inline("a\nb"), std::string("a\\nb"));
}
