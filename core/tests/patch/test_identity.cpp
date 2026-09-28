// SPDX-License-Identifier: GPL-3.0-or-later
#include <set>
#include <string>

#include "bambi/patch/identity.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("uuid: format and round trip") {
    const Uuid u = Uuid::generate();
    const std::string s = u.toString();

    CHECK(s.size() == 36);
    CHECK(s[8] == '-');
    CHECK(s[13] == '-');
    CHECK(s[18] == '-');
    CHECK(s[23] == '-');
    CHECK(s[14] == '4');                                                    // version 4
    CHECK((s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b'));  // RFC 4122

    const auto back = Uuid::parse(s);
    REQUIRE(back.has_value());
    CHECK(*back == u);
    CHECK(back->toString() == s);
}

TEST_CASE("uuid: parse rejects malformed input rather than guessing") {
    CHECK_FALSE(Uuid::parse("").has_value());
    CHECK_FALSE(Uuid::parse("not-a-uuid").has_value());
    CHECK_FALSE(Uuid::parse("00000000000000000000000000000000").has_value());      // no dashes
    CHECK_FALSE(Uuid::parse("0000000g-0000-4000-8000-000000000000").has_value());  // bad hex
    CHECK_FALSE(Uuid::parse("00000000-0000-4000-8000-00000000000").has_value());   // short
    CHECK_FALSE(Uuid::parse("00000000_0000-4000-8000-000000000000").has_value());  // bad sep
    CHECK(Uuid::parse("00000000-0000-4000-8000-000000000000").has_value());
}

TEST_CASE("uuid: nil is distinguishable and never generated") {
    CHECK(Uuid::nil().isNil());
    CHECK(Uuid::nil().toString() == "00000000-0000-0000-0000-000000000000");
    for (int i = 0; i < 200; ++i) CHECK_FALSE(Uuid::generate().isNil());
}

TEST_CASE("uuid: generation is unique across many instances in one process") {
    // A plugin gets instantiated dozens of times per session; collisions here would make
    // two sources indistinguishable on the link bus.
    std::set<std::string> seen;
    for (int i = 0; i < 5000; ++i) CHECK(seen.insert(Uuid::generate().toString()).second);
}

TEST_CASE("identity: a fresh one has distinct session and instance") {
    const Identity a = Identity::create();
    CHECK_FALSE(a.session.isNil());
    CHECK_FALSE(a.instance.isNil());
    CHECK_FALSE(a.session == a.instance);
    CHECK(a.label.empty());
    CHECK(a.colour >= 0);
    CHECK(a.colour < kNumColours);

    const Identity b = Identity::create();
    CHECK_FALSE(a.instance == b.instance);
}
