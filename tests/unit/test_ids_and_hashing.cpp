// Fabric Efficiency Ledger - identity derivation and hashing primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <string>
#include <vector>

#include "fel/concurrency.hpp"
#include "fel/crypto.hpp"
#include "fel/id.hpp"
#include "support/test_framework.hpp"

using namespace fel;

FEL_TEST(unit, sha256_known_answer_vectors) {
  FEL_EXPECT_EQ(Sha256::hash("").to_hex(),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FEL_EXPECT_EQ(Sha256::hash("abc").to_hex(),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FEL_EXPECT_EQ(
      Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

FEL_TEST(unit, sha256_one_million_characters) {
  Sha256 hasher;
  const std::string block(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    hasher.update(block);
  }
  FEL_EXPECT_EQ(hasher.finish().to_hex(),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FEL_TEST(unit, digest_hex_round_trip_and_rejection) {
  const Digest256 digest = Sha256::hash("fabric efficiency ledger");
  const std::string hex = digest.to_hex();
  FEL_EXPECT_EQ(hex.size(), std::size_t{64});
  FEL_EXPECT_EQ(Digest256::from_hex(hex), digest);
  FEL_EXPECT(is_hex(hex, 64));
  FEL_EXPECT(!is_hex(hex, 32));
  FEL_EXPECT(!Digest256::from_hex("short").to_hex().empty());
  FEL_EXPECT_EQ(Digest256::from_hex("not-hex-at-all-not-hex-at-all-not-hex-at-all-not-hex-0000"),
                Digest256{});
}

FEL_TEST(unit, crc32c_known_answers) {
  FEL_EXPECT_EQ(crc32c("123456789"), std::uint32_t{0xE3069283U});
  FEL_EXPECT_EQ(crc32c(""), std::uint32_t{0});
}

FEL_TEST(unit, strong_id_derivation_is_injective_and_stable) {
  const auto first = ResourceId::derive({"resource", "port-1"});
  const auto second = ResourceId::derive({"resource", "port-1"});
  const auto other = ResourceId::derive({"resource", "port-2"});
  FEL_EXPECT_EQ(first, second);
  FEL_EXPECT(first != other);
  // Field framing matters: a different split of the same characters is a
  // different identity.
  FEL_EXPECT(ResourceId::derive({"ab", "c"}) != ResourceId::derive({"a", "bc"}));
  FEL_EXPECT(!first.is_nil());
  FEL_EXPECT(ResourceId::nil().is_nil());
}

FEL_TEST(unit, strong_id_parse_round_trip_and_rejection) {
  const auto id = ScopeId::derive({"scope", "fabric"});
  const std::string text = id.to_string();
  FEL_EXPECT_EQ(text.size(), std::size_t{32});
  auto parsed = ScopeId::parse(text);
  FEL_REQUIRE(parsed.has_value());
  FEL_EXPECT_EQ(parsed.value(), id);
  FEL_EXPECT_EQ(ScopeId::parse("tooshort").error().code, ErrorCode::MalformedIdentity);
  FEL_EXPECT_EQ(ScopeId::parse(std::string(31, 'a')).error().code, ErrorCode::MalformedIdentity);
  FEL_EXPECT_EQ(ScopeId::parse(std::string(33, 'a')).error().code, ErrorCode::MalformedIdentity);
  FEL_EXPECT_EQ(ScopeId::parse(std::string(32, 'z')).error().code, ErrorCode::MalformedIdentity);
}

FEL_TEST(unit, strong_id_ordering_is_a_strict_weak_ordering) {
  std::vector<ResourceId> ids;
  for (int i = 0; i < 16; ++i) {
    ids.push_back(ResourceId::derive({"resource", std::to_string(i)}));
  }
  XorShift64 rng(0x1234ULL);
  for (std::size_t i = ids.size(); i > 1; --i) {
    const std::size_t j = static_cast<std::size_t>(rng.below(i));
    std::swap(ids[i - 1], ids[j]);
  }
  std::sort(ids.begin(), ids.end());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (std::size_t j = 0; j < ids.size(); ++j) {
      const bool ij = ids[i] < ids[j];
      const bool ji = ids[j] < ids[i];
      FEL_EXPECT(!(ij && ji));
      if (i == j) {
        FEL_EXPECT(!ij);
      }
    }
  }
  FEL_EXPECT(std::is_sorted(ids.begin(), ids.end()));
}

FEL_TEST(unit, accounting_identity_round_trip) {
  const AccountingIdentity identity = AccountingIdentity::from_digest(Sha256::hash("cell"));
  const std::string text = identity.to_string();
  FEL_EXPECT_EQ(text.size(), std::size_t{64});
  auto parsed = AccountingIdentity::parse(text);
  FEL_REQUIRE(parsed.has_value());
  FEL_EXPECT_EQ(parsed.value(), identity);
  FEL_EXPECT_EQ(AccountingIdentity::parse(std::string(63, 'a')).error().code,
                ErrorCode::MalformedIdentity);
}

FEL_TEST(unit, typed_identifiers_do_not_alias_across_domains) {
  // The tag name participates in the derivation, so the same textual parts in
  // two different domains can never produce the same raw value.
  const auto resource = ResourceId::derive({"x"});
  const auto scope = ScopeId::derive({"x"});
  FEL_EXPECT(resource.raw() != scope.raw());
}
