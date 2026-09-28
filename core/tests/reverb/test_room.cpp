// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <initializer_list>

#include "bambi/reverb/room.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
RoomSettings hall() { return {20.0, RoomShape::Hall, 1.9, 0.5, 0.6}; }
}  // namespace

TEST_CASE("a room is a box with its depth along front, and Eyring runs both ways") {
    const Room r = deriveRoom(hall());
    CHECK(r.y == 20.0);                          // width
    CHECK(r.x == doctest::Approx(36.0));         // depth, 1.8 x -- along x, which is front
    CHECK(r.z == doctest::Approx(12.0));         // height, 0.6 x
    CHECK(r.volume == doctest::Approx(8640.0));  // the hall the line lengths were tuned in
    CHECK(r.surface == doctest::Approx(2.0 * (36.0 * 20.0 + 36.0 * 12.0 + 20.0 * 12.0)));

    //  the absorption that gives a decay, put back, gives the decay
    for (const double rt : {0.3, 1.9, 6.5}) {
        const double a = eyringAbsorption(r.volume, r.surface, rt);
        CHECK(a > 0.0);
        CHECK(a < 1.0);
        CHECK(eyringDecay(r.volume, r.surface, a) == doctest::Approx(rt));
    }
    CHECK(r.absorbMid == doctest::Approx(1.0 - std::exp(-0.161 * 8640.0 / (r.surface * 1.9))));
}

TEST_CASE("tone shortens the highs and lengthens the lows, and the air takes its share of the highs") {
    RoomSettings s = hall();
    s.tone = 0.0;
    const Room dark = deriveRoom(s);
    CHECK(dark.rtLow == doctest::Approx(1.3 * 1.9));
    CHECK(dark.rtHigh == doctest::Approx(1.0 / (1.0 / (0.35 * 1.9) + 1.0 / 10.0)));
    s.tone = 1.0;
    const Room even = deriveRoom(s);
    CHECK(even.rtLow == doctest::Approx(1.9));
    //  even with walls that treat the highs like the mids, the air does not: shorter, never equal
    CHECK(even.rtHigh == doctest::Approx(1.0 / (1.0 / 1.9 + 1.0 / 10.0)));
    CHECK(even.rtHigh < even.rtMid);
    //  the walls' own absorption of the highs leaves the air out -- a reflection's path adds it
    CHECK(even.absorbHigh == doctest::Approx(even.absorbMid));
    CHECK(dark.absorbHigh > dark.absorbMid);
    CHECK(dark.absorbLow < dark.absorbMid);
}

TEST_CASE("a room cannot be drier than its walls allow, and says when it was asked to be") {
    RoomSettings s{5.0, RoomShape::Room, 0.01, 0.5, 0.5};
    const Room r = deriveRoom(s);
    CHECK(r.tooDry);
    CHECK(r.rtMid == doctest::Approx(r.minDecay));
    CHECK(r.minDecay == doctest::Approx(eyringDecay(r.volume, r.surface, 0.95)));
    CHECK(r.absorbMid == doctest::Approx(0.95));
    CHECK_FALSE(deriveRoom(hall()).tooDry);
}

TEST_CASE("the mixing time and the lines follow the volume, between the limits the ear set") {
    //  sqrt(V) ms between 25 and 80; 1.2 * cbrt(V / 8640) between 0.55 and 2
    const Room small = deriveRoom({4.0, RoomShape::Room, 0.35, 0.5, 0.8});  // 52 m3
    const Room mid = deriveRoom({10.0, RoomShape::Room, 1.0, 0.5, 0.5});    // 812 m3
    const Room big = deriveRoom({38.0, RoomShape::Tall, 6.5, 0.35, 0.5});   // 72,000 m3
    CHECK(small.mixingTime == 0.025);
    CHECK(mid.mixingTime == doctest::Approx(std::sqrt(812.5) / 1000.0));
    CHECK(big.mixingTime == 0.080);
    CHECK(small.lineScale == 0.55);
    CHECK(deriveRoom(hall()).lineScale == doctest::Approx(1.2));
    CHECK(big.lineScale == 2.0);
    //  between the limits it goes as the cube root: a room a third the hall's volume has lines 0.84 as long
    const Room third = deriveRoom({14.0, RoomShape::Hall, 1.5, 0.5, 0.5});
    CHECK(third.lineScale == doctest::Approx(1.2 * std::cbrt(14.0 * 25.2 * 8.4 / 8640.0)));
    CHECK(third.lineScale == doctest::Approx(0.84).epsilon(0.01));
    //  roughness is one axis: it scatters the reflections and diffuses the onset
    CHECK(deriveRoom(hall()).scatter == doctest::Approx(0.48));
    CHECK(deriveRoom(hall()).diffusion == doctest::Approx(0.6));
}
