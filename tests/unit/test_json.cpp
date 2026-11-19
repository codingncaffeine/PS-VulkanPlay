#include <string>

#include "app/json.h"
#include "check.h"

using psvp::app::Json;

PSVP_TEST(json_reads_every_kind_of_value) {
    const auto json = Json::parse(R"({"text":"hi","count":42,"ratio":-1.5e2,"yes":true,"no":false,"nothing":null,
                                      "list":[1,"two",[3]],"inner":{"deep":"value"}})");
    CHECK(json.has_value());
    if (!json) return;
    CHECK(json->string_or("text", "") == "hi");
    CHECK(json->number_or("count", 0) == 42);
    CHECK(json->number_or("ratio", 0) == -150);
    CHECK(json->find("yes") != nullptr && *json->find("yes")->as_bool());
    CHECK(json->find("no") != nullptr && !*json->find("no")->as_bool());
    CHECK(json->find("nothing") != nullptr && json->find("nothing")->is_null());
    CHECK(json->find("list")->as_array()->size() == 3);
    CHECK(json->find("inner")->string_or("deep", "") == "value");
    CHECK(json->find("absent") == nullptr);
    CHECK(json->string_or("count", "fallback") == "fallback");
    CHECK(json->number_or("text", 7) == 7);
}

PSVP_TEST(json_reads_string_escapes) {
    const auto json = Json::parse(R"(["quote \" slash \\ \/ tab \t line \n", "\u0041\u00fc\u20ac", "\ud83d\ude00"])");
    CHECK(json.has_value());
    if (!json) return;
    const auto& items = *json->as_array();
    CHECK(*items[0].as_string() == "quote \" slash \\ / tab \t line \n");
    CHECK(*items[1].as_string() == "A\xc3\xbc\xe2\x82\xac");
    CHECK(*items[2].as_string() == "\xf0\x9f\x98\x80");
}

PSVP_TEST(json_refuses_what_is_not_json) {
    for (const char* text : {"", "{", "}", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}", "[1 2]", "tru", "01", "1.", "-",
                             "1e", "\"open", "\"bad \\q escape\"", "\"\\ud83d alone\"", "\"\\ude00\"", "[1] trailing",
                             "\"tab\tinside\"", "nul"}) {
        CHECK(!Json::parse(text).has_value());
    }
}

PSVP_TEST(json_refuses_nesting_that_is_too_deep) {
    const std::string deep_ok = std::string(Json::kMaxDepth, '[') + std::string(Json::kMaxDepth, ']');
    const std::string too_deep = std::string(Json::kMaxDepth + 2, '[') + std::string(Json::kMaxDepth + 2, ']');
    CHECK(Json::parse(deep_ok).has_value());
    CHECK(!Json::parse(too_deep).has_value());
    CHECK(!Json::parse(std::string(100000, '[')).has_value());
}

PSVP_TEST(json_writes_what_it_reads) {
    const Json document(Json::Object{
        {"name", Json("Living Room")},
        {"odd", Json("quote \" backslash \\ newline \n tab \t bell \a")},
        {"count", Json(std::int64_t{3})},
        {"half", Json(0.5)},
        {"flag", Json(true)},
        {"none", Json(nullptr)},
        {"empty_list", Json(Json::Array{})},
        {"empty_object", Json(Json::Object{})},
        {"list", Json(Json::Array{Json(std::int64_t{1}), Json("two")})},
    });
    const std::string text = document.dump();
    CHECK(text.ends_with("}\n"));
    CHECK(text.find("\"count\": 3,") != std::string::npos);
    CHECK(text.find("\"empty_list\": []") != std::string::npos);
    const auto back = Json::parse(text);
    CHECK(back.has_value());
    if (!back) return;
    CHECK(back->dump() == text);
    CHECK(back->string_or("odd", "") == "quote \" backslash \\ newline \n tab \t bell \a");
    CHECK(back->number_or("half", 0) == 0.5);
}

PSVP_TEST(json_keeps_members_in_order_and_finds_the_first) {
    const auto json = Json::parse(R"({"b":1,"a":2,"b":3})");
    CHECK(json.has_value());
    if (!json) return;
    CHECK(json->as_object()->size() == 3);
    CHECK((*json->as_object())[0].first == "b");
    CHECK(json->number_or("b", 0) == 1);
}

// Found by the fuzzer: a number must read back as itself after being written,
// however long or small it is.
PSVP_TEST(json_numbers_survive_a_round_trip_exactly) {
    for (const char* text : {"43333333333333333333333333333333333333333333333333333333333333333333333", "0.1", "-0.30000000000000004",
                             "1e-320", "2.2250738585072014e-308", "1.7976931348623157e308", "9007199254740993", "123456789012345678",
                             "5e-324", "0.000001", "-0", "1E5"}) {
        const auto first = Json::parse(text);
        CHECK(first.has_value());
        if (!first) continue;
        const std::string written = first->dump();
        const auto second = Json::parse(written);
        CHECK(second.has_value() && *second->as_number() == *first->as_number());
        CHECK(second.has_value() && second->dump() == written);
    }
    CHECK(*Json::parse("0.1").value().as_number() == 0.1);
    CHECK(*Json::parse("1e3").value().as_number() == 1000.0);
    CHECK(!Json::parse("1e999").has_value());
    CHECK(!Json::parse("-1e999").has_value());
}
