// SPDX-License-Identifier: MIT
#include "obsn/json.hpp"
#include "obsn_test.hpp"

using namespace obsn;

TEST(json, parses_scalars_and_containers) {
    const auto r = json::parse(R"({"a":1,"b":true,"c":null,"d":[1,2,3],"e":{"f":"g"}})");
    CHECK(r.ok);
    CHECK_EQ(r.value.get_int("a"), 1);
    CHECK_EQ(r.value.get_bool("b"), true);
    CHECK(r.value.find("c")->is_null());
    CHECK_EQ(r.value.find("d")->as_array().size(), std::size_t{3});
    CHECK_EQ(r.value.find("e")->get_string("f"), std::string("g"));
}

TEST(json, round_trips_unicode_and_escapes) {
    const std::string source = R"({"t":"line\nbreak \"quoted\" \u00e9 \ud83d\ude00"})";
    const auto r = json::parse(source);
    CHECK(r.ok);
    const std::string text = r.value.get_string("t");
    CHECK(text.find('\n') != std::string::npos);
    CHECK(text.find("\xC3\xA9") != std::string::npos);      // é
    CHECK(text.find("\xF0\x9F\x98\x80") != std::string::npos);  // U+1F600 via surrogate pair
    const auto again = json::parse(r.value.dump());
    CHECK(again.ok);
    CHECK_EQ(again.value.get_string("t"), text);
}

TEST(json, rejects_malformed_documents) {
    const char* bad[] = {
        "{",  "}", "{\"a\":}", "{\"a\" 1}", "[1,]", "tru", "01", "1.", "\"unterminated",
        "{\"a\":1}trailing", "{\"a\":\"\\q\"}", "[1 2]",
    };
    for (const char* text : bad) {
        const auto r = json::parse(text);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
    }
}

TEST(json, rejects_unpaired_surrogates) {
    CHECK(!json::parse(R"({"t":"\ud83d"})").ok);
    CHECK(!json::parse(R"({"t":"\ude00"})").ok);
}

TEST(json, enforces_depth_limit) {
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "[";
    for (int i = 0; i < 40; ++i) deep += "]";
    json::Limits limits;
    limits.max_depth = 16;
    const auto r = json::parse(deep, limits);
    CHECK(!r.ok);
    CHECK(r.error.find("max_depth") != std::string::npos);
}

TEST(json, enforces_size_and_element_limits) {
    json::Limits limits;
    limits.max_total_bytes = 32;
    CHECK(!json::parse(std::string(64, 'a'), limits).ok);

    std::string many = "[";
    for (int i = 0; i < 200; ++i) many += (i ? ",1" : "1");
    many += "]";
    json::Limits element_limit;
    element_limit.max_array_elements = 100;
    CHECK(!json::parse(many, element_limit).ok);
}

TEST(json, rejects_invalid_utf8_input) {
    const std::string bad = std::string("{\"t\":\"") + "\xFF\xFE" + "\"}";
    CHECK(!json::parse(bad).ok);
}

TEST(json, serialiser_repairs_invalid_utf8) {
    // Damaged input must still produce a document a conformant parser can read back.
    std::string out;
    json::escape_string(std::string("ok\xC3\x28" "end"), out);
    CHECK(json::is_valid_utf8(out));
    CHECK(out.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD replacement
}

TEST(json, control_characters_are_escaped) {
    json::Value v{json::Object{}};
    v.set("t", json::Value(std::string("a\x01" "b")));
    const std::string dumped = v.dump();
    CHECK(dumped.find("\\u0001") != std::string::npos);
    CHECK(json::parse(dumped).ok);
}

TEST(json, accessors_never_throw_on_type_mismatch) {
    const auto r = json::parse(R"({"n":"not a number","a":5})");
    CHECK(r.ok);
    CHECK_EQ(r.value.get_int("n", 42), 42);
    CHECK_EQ(r.value.get_string("a", "fallback"), std::string("fallback"));
    CHECK_EQ(r.value.get_bool("missing", true), true);
    CHECK_EQ(r.value.find("missing"), nullptr);
}

TEST(json, truncate_utf8_does_not_split_sequences) {
    const std::string source = "a\xC3\xA9\xF0\x9F\x98\x80z";  // a, é, emoji, z
    CHECK_EQ(json::truncate_utf8(source, 2), std::string("a\xC3\xA9"));
    CHECK_EQ(json::truncate_utf8(source, 3), std::string("a\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK(json::is_valid_utf8(json::truncate_utf8(source, 3)));
}

TEST(json, set_replaces_rather_than_duplicates) {
    json::Value v{json::Object{}};
    v.set("k", json::Value(1));
    v.set("k", json::Value(2));
    CHECK_EQ(v.as_object().size(), std::size_t{1});
    CHECK_EQ(v.get_int("k"), 2);
}

TEST(json, large_integers_survive_a_round_trip) {
    json::Value v{json::Object{}};
    v.set("ts", json::Value(static_cast<long long>(1737072000123LL)));
    const auto r = json::parse(v.dump());
    CHECK(r.ok);
    CHECK_EQ(r.value.get_int("ts"), 1737072000123LL);
}
