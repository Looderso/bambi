// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/decode/allrad.hpp"
#include "bambi/decode/layouts.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("the BS.2051 layouts are there by name, channels as the recommendation lists them, the LFE a gap") {
    //  Catches: a table with a channel out of order, an LFE given a loudspeaker, or an elevation sign slipped.
    CHECK(bs2051Names().size() == 10);
    CHECK(bs2051Layout("5.1") == nullptr);
    const Layout* five = bs2051Layout("0+5+0");
    REQUIRE(five != nullptr);
    REQUIRE(five->speakers.size() == 5);           // six channels, the fourth the LFE
    CHECK(five->speakers[0].azimuthDeg == 30.0);   // L
    CHECK(five->speakers[1].azimuthDeg == -30.0);  // R
    CHECK(five->speakers[2].azimuthDeg == 0.0);    // C
    CHECK(five->speakers[3].channel == 5);         // Ls, after the LFE's gap
    CHECK(five->speakers[3].azimuthDeg == 110.0);
    CHECK(bs2051ChannelName("0+5+0", 4) == "LFE1");
    CHECK(bs2051ChannelName("4+7+0", 9) == "U+045");
    const Layout* j = bs2051Layout("4+7+0");
    REQUIRE(j != nullptr);
    CHECK(j->speakers.size() == 11);
    CHECK(j->speakers[7].elevationDeg == 30.0);
    const Layout* big = bs2051Layout("9+10+3");
    REQUIRE(big != nullptr);
    int below = 0, top = 0;
    for (const Loudspeaker& s : big->speakers) {
        if (s.elevationDeg < 0) ++below;
        if (s.elevationDeg == 90.0) ++top;
    }
    CHECK(below == 3);
    CHECK(top == 1);
}

TEST_CASE("every BS.2051 layout but the stereo pair designs a decoder at orders 1 to 3") {
    //  Catches: a built-in layout the hull or the ring refuses -- the pair is the one that leaves half a turn open.
    for (std::string_view name : bs2051Names()) {
        const Layout* l = bs2051Layout(name);
        REQUIRE(l != nullptr);
        for (int order : {1, 2, 3}) {
            SpeakerDesign d;
            std::string error;
            const bool ok = designAllrad(*l, order, Weighting::Basic, d, &error);
            if (name == "0+2+0") {
                CHECK_FALSE(ok);
            } else {
                CHECK_MESSAGE(ok, std::string(name) << " at order " << order << ": " << error);
                CHECK(d.outputs == static_cast<int>(l->speakers.back().channel));
                CHECK(d.ring == (name.substr(0, 2) == "0+"));  // 5.1 and 7.1: nothing above or below
            }
        }
    }
}
