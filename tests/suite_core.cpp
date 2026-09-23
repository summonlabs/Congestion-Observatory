// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace congestion;

CO_TEST(core, name_accepts_canonical_text) {
  for (const char* text : {"leaf1", "leaf1/2", "queue:3", "tenant-a", "a.b.c", "A-Z_09", "x=y+z",
                           "ns@host#1", "a,b|c"}) {
    auto parsed = Name::parse(text);
    CO_EXPECT(parsed.ok());
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value().str(), std::string(text));
    }
  }
  CO_EXPECT_EQ(Name::kMaxLength, static_cast<std::size_t>(96));
}

CO_TEST(core, name_rejects_non_canonical_text) {
  std::string too_long(Name::kMaxLength + 1, 'a');
  const char* invalid[] = {"",        " leaf",   "leaf ",   "leaf 1",  "/leaf",  "leaf/",
                           "a//b",    "a::b",    "leaf\t1", "leaf\n1", "caf\xc3\xa9", "a;b",
                           "a\"b",    "a'b",     "a(b)",    "a[b]",    "a\\b",   "$leaf"};
  for (const char* text : invalid) {
    CO_EXPECT(!Name::parse(text).ok());
  }
  CO_EXPECT(!Name::parse(too_long).ok());
}

CO_TEST(core, entity_ids_are_typed_and_ordered) {
  auto link = LinkId::parse("leaf1-leaf2");
  auto port = PortId::parse("leaf1/2");
  CO_EXPECT(link.ok());
  CO_EXPECT(port.ok());
  CO_EXPECT_EQ(link.value().str(), std::string("leaf1-leaf2"));
  CO_EXPECT(link.value() < LinkId::unchecked("leaf1-leaf3"));
  CO_EXPECT(link.value() != LinkId::unchecked("leaf1-leaf3"));
  CO_EXPECT(!LinkId::parse("").ok());
  CO_EXPECT(!LinkId{}.valid());
}

CO_TEST(core, digest_ids_round_trip) {
  const auto id = EvidenceId::from_digest({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
  CO_EXPECT_EQ(id.str(), std::string("0123456789abcdeffedcba9876543210"));
  auto parsed = EvidenceId::parse(id.str());
  CO_EXPECT(parsed.ok());
  CO_EXPECT_EQ(parsed.value(), id);
  CO_EXPECT(!EvidenceId::parse("0123456789ABCDEFFEDCBA9876543210").ok());
  CO_EXPECT(!EvidenceId::parse("0123").ok());
  CO_EXPECT(!EvidenceId{}.valid());
}

CO_TEST(core, timestamps_round_trip_through_iso8601) {
  const std::int64_t samples[] = {0,
                                  1,
                                  999999999,
                                  1000000000,
                                  -1,
                                  -1500000000,
                                  951782400000000000LL,   /* 2000-02-29 */
                                  4102444800000000000LL}; /* 2100-01-01 */
  for (const std::int64_t nanos : samples) {
    const Timestamp stamp(nanos);
    const std::string text = stamp.to_iso8601();
    auto parsed = Timestamp::from_iso8601(text);
    CO_EXPECT(parsed.ok());
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value().unix_nanos(), nanos);
    }
  }
  CO_EXPECT_EQ(Timestamp::from_unix_seconds(0).value().to_iso8601(), std::string("1970-01-01T00:00:00Z"));
}

CO_TEST(core, timestamps_reject_malformed_text) {
  const char* invalid[] = {"", "1970-01-01T00:00:00", "1970-01-01 00:00:00Z",
                           "1970-13-01T00:00:00Z", "1970-01-32T00:00:00Z",
                           "1970-01-01T24:00:00Z", "1970-01-01T00:60:00Z",
                           "1970-01-01T00:00:61Z", "1971-02-29T00:00:00Z",
                           "1970-01-01T00:00:00+01:00"};
  for (const char* text : invalid) {
    CO_EXPECT(!Timestamp::from_iso8601(text).ok());
  }
  CO_EXPECT(Timestamp::from_iso8601("1970-01-01T00:00:00.123456789Z").ok());
  CO_EXPECT_EQ(Timestamp::from_iso8601("1970-01-01T00:00:00.123456789Z").value().unix_nanos(),
               static_cast<std::int64_t>(123456789));
}

CO_TEST(core, timestamp_conversion_detects_overflow) {
  CO_EXPECT(Timestamp::from_unix_seconds(std::numeric_limits<std::int64_t>::max() / 1000000000).ok());
  CO_EXPECT(!Timestamp::from_unix_seconds(std::numeric_limits<std::int64_t>::max()).ok());
  CO_EXPECT(!Timestamp::from_unix_millis(std::numeric_limits<std::int64_t>::max()).ok());
  const auto error = Timestamp::from_unix_seconds(std::numeric_limits<std::int64_t>::max());
  CO_EXPECT_ERR(Timestamp::from_unix_seconds(std::numeric_limits<std::int64_t>::max()),
                ErrorCode::kArithmeticOverflow);
  CO_EXPECT_EQ(error.code(), ErrorCode::kArithmeticOverflow);
}

CO_TEST(core, duration_rendering_is_stable) {
  CO_EXPECT_EQ(Duration::from_seconds(2).to_string(), std::string("2s"));
  CO_EXPECT_EQ(Duration::from_millis(1500).to_string(), std::string("1.5s"));
  CO_EXPECT_EQ(Duration::from_nanos(1).to_string(), std::string("0.000000001s"));
  CO_EXPECT_EQ(Duration::from_nanos(-1).to_string(), std::string("-0.000000001s"));
  CO_EXPECT_EQ(Duration::zero().to_string(), std::string("0s"));
}

CO_TEST(core, checked_arithmetic_reports_overflow) {
  std::uint64_t unsigned_sum = 0;
  CO_EXPECT(checked_add<std::uint64_t>(1, 2, unsigned_sum));
  CO_EXPECT_EQ(unsigned_sum, static_cast<std::uint64_t>(3));
  CO_EXPECT(!checked_add<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 1, unsigned_sum));
  std::int64_t signed_sum = 0;
  CO_EXPECT(checked_add<std::int64_t>(std::numeric_limits<std::int64_t>::max(), 1, signed_sum) == false);
  CO_EXPECT(checked_add<std::int64_t>(std::numeric_limits<std::int64_t>::min(), -1, signed_sum) == false);
  std::uint64_t product = 0;
  CO_EXPECT(checked_mul<std::uint64_t>(1000, 1000, product));
  CO_EXPECT_EQ(product, static_cast<std::uint64_t>(1000000));
  CO_EXPECT(!checked_mul<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 2, product));
  std::int64_t signed_product = 0;
  CO_EXPECT(!checked_mul<std::int64_t>(-1, std::numeric_limits<std::int64_t>::min(), signed_product));
  auto bounded = checked_add_within(10, 5, 20, "bytes");
  CO_EXPECT(bounded.ok());
  CO_EXPECT_ERR(checked_add_within(10, 15, 20, "bytes"), ErrorCode::kLimitExceeded);
  CO_EXPECT_ERR(checked_add_within(std::numeric_limits<std::uint64_t>::max(), 1,
                                   std::numeric_limits<std::uint64_t>::max(), "bytes"),
                ErrorCode::kArithmeticOverflow);
}

CO_TEST(core, stable_hash_is_chunking_independent) {
  StableHasher single;
  single.update("abcdef");
  StableHasher chunked;
  chunked.update("abc");
  chunked.update("def");
  CO_EXPECT_EQ(single.digest64(), chunked.digest64());
  CO_EXPECT_EQ(single.digest128(), chunked.digest128());

  StableHasher other;
  other.update("abc");
  other.update("defg");
  CO_EXPECT_NE(single.digest64(), other.digest64());

  StableHasher little_endian;
  little_endian.update_u32(1);
  StableHasher big_endian;
  big_endian.update_u32(0x01000000u);
  CO_EXPECT_NE(little_endian.digest64(), big_endian.digest64());
  CO_EXPECT_EQ(to_hex(0x0123456789abcdefULL), std::string("0123456789abcdef"));
}

CO_TEST(core, crc64_matches_the_published_check_value) {
  // CRC-64/XZ check value for the ASCII string "123456789".
  CO_EXPECT_EQ(crc64("123456789"), 0x995DC9BBDF1939FAULL);
  CO_EXPECT_EQ(crc64(""), 0x0000000000000000ULL);
  Crc64 incremental;
  incremental.update("1234");
  incremental.update("56789");
  CO_EXPECT_EQ(incremental.value(), crc64("123456789"));
}

CO_TEST(core, limits_validation_and_digest) {
  Limits limits;
  CO_EXPECT(limits.validate().ok());
  const std::uint64_t digest = limits.digest();
  Limits changed = limits;
  changed.max_retained_evidence += 1;
  CO_EXPECT_NE(changed.digest(), digest);
  Limits broken = limits;
  broken.max_sources = 0;
  CO_EXPECT_STATUS_ERR(broken.validate(), ErrorCode::kInvalidArgument);
  broken = limits;
  broken.max_evidence_per_subject = limits.max_retained_evidence + 1;
  CO_EXPECT_STATUS_ERR(broken.validate(), ErrorCode::kInvalidArgument);
  broken = limits;
  broken.worker_threads = 4096;
  CO_EXPECT_STATUS_ERR(broken.validate(), ErrorCode::kOutOfRange);
  CO_EXPECT(minimal_limits().validate().ok());
  CO_EXPECT_NE(minimal_limits().digest(), limits.digest());
  CO_EXPECT(limits.describe().find("max_retained_evidence=") != std::string::npos);
}

CO_TEST(core, cancellation_is_monotonic) {
  CancellationSource source;
  const CancellationToken token = source.token();
  CO_EXPECT(!token.cancelled());
  CO_EXPECT(token.reason().empty());
  source.cancel("first");
  CO_EXPECT(token.cancelled());
  CO_EXPECT_EQ(token.reason(), std::string("first"));
  source.cancel("second");
  CO_EXPECT_EQ(token.reason(), std::string("first"));
  CancellationToken detached;
  CO_EXPECT(!detached.cancelled());
  CO_EXPECT(detached.reason().empty());
}

CO_TEST(core, json_writer_preserves_member_order) {
  JsonObject root;
  root.set("zulu", JsonValue(static_cast<std::int64_t>(1)));
  root.set("alpha", JsonValue("two"));
  root.set("middle", JsonValue(true));
  JsonValue::Array items;
  items.push_back(JsonValue(1.5));
  items.push_back(JsonValue(nullptr));
  root.set("list", JsonValue(std::move(items)));
  const std::string text = JsonValue(std::move(root)).dump(0);
  CO_EXPECT_EQ(text, std::string("{\"zulu\":1,\"alpha\":\"two\",\"middle\":true,\"list\":[1.5,null]}"));
}

CO_TEST(core, json_parser_round_trips_and_rejects_hostile_input) {
  const std::string document =
      "{\"a\":[1,2,3],\"b\":{\"c\":\"text\\nwith\\tescapes\",\"d\":-12.5,\"e\":true,\"f\":null}}";
  auto parsed = parse_json(document, Limits{});
  CO_EXPECT(parsed.ok());
  if (parsed.ok()) {
    CO_EXPECT(parsed.value().is_object());
    CO_EXPECT_EQ(parsed.value().member("a")->as_array().size(), static_cast<std::size_t>(3));
    CO_EXPECT_EQ(parsed.value().member("b")->member("c")->as_string(),
                 std::string("text\nwith\tescapes"));
    CO_EXPECT_EQ(parsed.value().member("b")->member("d")->as_double(), -12.5);
    CO_EXPECT_EQ(parsed.value().dump(0), document);

    const char* invalid[] = {"", "{", "[]]", "{\"a\":}", "{\"a\":1,}", "{'a':1}", "{\"a\":01x}",
                             "{\"a\":1}{}", "nul", "{\"a\":1,\"a\":2}", "{\"a\":NaN}",
                             "{\"a\":1,}", "[1,]", "{\"a\":1}trailing"};
    for (const char* text : invalid) {
      CO_EXPECT(!parse_json(text, Limits{}).ok());
    }
  }
}

CO_TEST(core, json_parser_enforces_bounds) {
  Limits limits;
  limits.max_json_depth = 4;
  limits.max_json_nodes = 16;
  limits.max_document_bytes = 64;
  CO_EXPECT(parse_json("[[[[1]]]]", limits).ok());
  CO_EXPECT_ERR(parse_json("[[[[[1]]]]]", limits), ErrorCode::kLimitExceeded);
  CO_EXPECT_ERR(parse_json("[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18]", limits),
                ErrorCode::kLimitExceeded);
  CO_EXPECT_ERR(parse_json("\"" + std::string(200, 'x') + "\"", limits),
                ErrorCode::kLimitExceeded);
  CO_EXPECT_ERR(parse_json("99999999999999999999999999", limits), ErrorCode::kOutOfRange);
}

CO_TEST(core, json_handles_unicode_escapes) {
  auto parsed = parse_json("\"\\u0041\\u00e9\\ud83d\\ude00\"", Limits{});
  CO_EXPECT(parsed.ok());
  if (parsed.ok()) {
    CO_EXPECT_EQ(parsed.value().as_string(), std::string("A\xc3\xa9\xf0\x9f\x98\x80"));
  }
  CO_EXPECT(!parse_json("\"\\ud83d\"", Limits{}).ok());
  CO_EXPECT(!parse_json("\"\\ude00\"", Limits{}).ok());
  CO_EXPECT(!parse_json("\"\\uZZZZ\"", Limits{}).ok());
  CO_EXPECT(!parse_json("\"\\q\"", Limits{}).ok());
}

CO_TEST(core, json_escape_covers_control_characters) {
  CO_EXPECT_EQ(json_escape("plain"), std::string("plain"));
  CO_EXPECT_EQ(json_escape("a\"b"), std::string("a\\\"b"));
  CO_EXPECT_EQ(json_escape("a\\b"), std::string("a\\\\b"));
  CO_EXPECT_EQ(json_escape(std::string("\x01")), std::string("\\u0001"));
}
