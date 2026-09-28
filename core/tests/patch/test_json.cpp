// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/patch/json.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
Json roundTrip(const Json& j) {
    std::string err;
    Json back = Json::parse(j.dump(), &err);
    REQUIRE(err.empty());
    return back;
}
}  // namespace

TEST_CASE("scalars round-trip") {
    CHECK(Json::parse("null").isNull());
    CHECK(Json::parse("true").boolOr(false) == true);
    CHECK(Json::parse("false").boolOr(true) == false);
    CHECK(Json::parse("42").numberOr(0) == doctest::Approx(42));
    CHECK(Json::parse("-1.5e3").numberOr(0) == doctest::Approx(-1500));
    CHECK(Json::parse("\"hi\"").stringOr("") == "hi");
}

TEST_CASE("numbers survive a round trip exactly") {
    // Preset values must not drift on every save/load cycle.
    const double vals[] = {0.0,     1.0, -1.0, 0.1, 1.0 / 3.0, 1e-9, 1e9, 3.141592653589793, -0.30000000000000004,
                           96000.0, 0.5, 180.0};
    for (double v : vals) {
        const Json j{v};
        INFO("value ", v, " dumped as ", j.dump(0));
        CHECK(roundTrip(j).numberOr(-999) == v);  // exact, not approximate
    }
}

TEST_CASE("strings: escapes and unicode") {
    const std::string tricky = "quote\" back\\ slash/ \b\f\n\r\t end";
    CHECK(roundTrip(Json{tricky}).stringOr("") == tricky);

    CHECK(Json::parse("\"\\u0041\"").stringOr("") == "A");
    CHECK(Json::parse("\"\\u00e9\"").stringOr("") == "\xc3\xa9");                 // e-acute
    CHECK(Json::parse("\"\\u20ac\"").stringOr("") == "\xe2\x82\xac");             // euro
    CHECK(Json::parse("\"\\ud83c\\udfb5\"").stringOr("") == "\xf0\x9f\x8e\xb5");  // surrogate pair

    const std::string ctrl = std::string(
        "a\x01"
        "b");
    CHECK(roundTrip(Json{ctrl}).stringOr("") == ctrl);
}

TEST_CASE("objects and arrays") {
    Json o = Json::object();
    o.set("name", Json{"rhodes"});
    o.set("count", Json{3});
    o.set("on", Json{true});
    Json a = Json::array();
    a.push(Json{1});
    a.push(Json{2});
    o.set("nums", a);

    const Json back = roundTrip(o);
    REQUIRE(back.isObject());
    CHECK(back.find("name")->stringOr("") == "rhodes");
    CHECK(back.find("count")->numberOr(0) == doctest::Approx(3));
    CHECK(back.find("on")->boolOr(false));
    REQUIRE(back.find("nums")->isArray());
    CHECK(back.find("nums")->size() == 2);
    CHECK(back.find("absent") == nullptr);
}

TEST_CASE("objects keep insertion order so presets diff cleanly") {
    Json o = Json::object();
    o.set("zebra", Json{1});
    o.set("apple", Json{2});
    o.set("mango", Json{3});
    REQUIRE(o.members().size() == 3);
    CHECK(o.members()[0].first == "zebra");
    CHECK(o.members()[1].first == "apple");
    CHECK(o.members()[2].first == "mango");

    o.set("apple", Json{9});  // replace in place, not append
    REQUIRE(o.members().size() == 3);
    CHECK(o.members()[1].first == "apple");
    CHECK(o.find("apple")->numberOr(0) == doctest::Approx(9));
}

TEST_CASE("reads never throw and degrade to the fallback") {
    // This is the property that makes loading an older or newer document a non-event.
    const Json s{"text"};
    CHECK(s.numberOr(7) == doctest::Approx(7));
    CHECK(s.boolOr(true) == true);
    CHECK(s.find("anything") == nullptr);
    CHECK(Json{}.stringOr("fallback") == "fallback");
    CHECK(Json{}.size() == 0);
}

TEST_CASE("malformed input fails with a message rather than silently") {
    const char* bad[] = {"",        "{",        "}",         "[1,2",           "[1,,2]",
                         "{\"a\"}", "{\"a\":}", "tru",       "\"unterminated", "{\"a\":1} trailing",
                         "[1] [2]", "\"\\q\"",  "\"\\u12\"", "01abc",          "--3"};
    for (const char* b : bad) {
        std::string err;
        const Json j = Json::parse(b, &err);
        INFO("input: ", b);
        CHECK(j.isNull());
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("deep nesting is refused rather than blowing the stack") {
    std::string deep(200, '[');
    deep += "1";
    deep.append(200, ']');
    std::string err;
    Json::parse(deep, &err);
    CHECK_FALSE(err.empty());
}

TEST_CASE("dump(0) is single-line, dump(2) is indented, both re-parse") {
    Json o = Json::object();
    o.set("a", Json{1});
    Json inner = Json::object();
    inner.set("b", Json{2});
    o.set("nested", inner);

    const std::string flat = o.dump(0);
    CHECK(flat.find('\n') == std::string::npos);
    const std::string pretty = o.dump(2);
    CHECK(pretty.find('\n') != std::string::npos);

    std::string e1, e2;
    CHECK(Json::parse(flat, &e1).find("nested")->find("b")->numberOr(0) == doctest::Approx(2));
    CHECK(Json::parse(pretty, &e2).find("nested")->find("b")->numberOr(0) == doctest::Approx(2));
    CHECK(e1.empty());
    CHECK(e2.empty());
}

TEST_CASE("empty containers") {
    CHECK(Json::array().dump(0) == "[]");
    CHECK(Json::object().dump(0) == "{}");
    CHECK(Json::parse("[]").isArray());
    CHECK(Json::parse("{}").isObject());
    CHECK(Json::parse("  [ ]  ").isArray());
}
