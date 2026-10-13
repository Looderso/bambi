// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <string_view>

#include "bambi/decode/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  The decoder's own keys, frozen: a host persists automation against them. The shared blocks follow and are
 *  frozen where they are defined. */
TEST_CASE("the decoder's own parameter keys are frozen, and its manifest holds together") {
    //  Catches: a key renamed or reordered, which would detach every saved automation lane.
    constexpr std::array<std::string_view, 4> kOwn{"head.yaw", "head.pitch", "head.roll", "output.level"};
    const ParamManifest& m = decodeParams();
    REQUIRE(m.size() >= static_cast<int>(kOwn.size()));
    for (std::size_t i = 0; i < kOwn.size(); ++i) CHECK(m[static_cast<int>(i)].key == kOwn[i]);
    CHECK(m.byKey("rates.retrigger") != kNoParam);
    CHECK(m.byKey("level.release") != kNoParam);
    CHECK(m.wrapsAt(m.byKey("head.yaw")));
    CHECK_FALSE(m.wrapsAt(m.byKey("head.pitch")));
    //  Catches: the rates slipped in before the shared blocks, which would move every shared key's index.
    CHECK(m.byKey("head.yaw_rate") > m.byKey("level.release"));
    CHECK(m.byKey("head.roll_rate") == m.size() - 1);
    CHECK(decodeMod().complete());
    CHECK(decodeMod().baseTargets.size() == 1);
}
