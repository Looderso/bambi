// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "bambi/echo/ping.hpp"
#include "bambi/echo/warp.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
TapSettings plainTap(Vec3 axis, double spinRad, double skew = 0.0) {
    TapSettings t;
    t.axis = axis;
    t.spinRad = spinRad;
    t.skew = skew;
    return t;
}
}  // namespace

TEST_CASE("the ping carries a direction the way a pass does") {
    SUBCASE("a spin about up turns the azimuth and leaves the elevation alone") {
        //  Catches: the spin dropped (the bead never moves) or its sign swapped.
        const TapSettings t = plainTap({0.0, 0.0, 1.0}, 90.0 * kDeg2Rad);
        const Vec3 front{1.0, 0.0, 0.0};
        const Vec3 after = pingStep(t, front);
        //  counter-clockwise seen from the tip of +z: front goes to LEFT, which is +y (x front, y left)
        CHECK(after.x == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(after.y == doctest::Approx(1.0).epsilon(1e-9));
        CHECK(after.z == doctest::Approx(0.0).epsilon(1e-9));

        //  and four of them come back to where they started
        Vec3 at = front;
        for (int i = 0; i < 4; ++i) at = pingStep(t, at);
        CHECK(at.x == doctest::Approx(1.0).epsilon(1e-9));

        /*  Which way it turns, at an angle and from starts where the sign actually shows: at 90
            degrees from front both signs give the same answer, because the term the sign is on is
            multiplied by zero there. Azimuth is counter-clockwise from front, so a positive spin
            about up raises it. */
        const TapSettings turn = plainTap({0.0, 0.0, 1.0}, 30.0 * kDeg2Rad);
        for (const double startDeg : {0.0, 90.0, -120.0}) {
            const Vec3 start = fromAzEl(startDeg * kDeg2Rad, 0.0);
            const Vec3 landed = pingStep(turn, start);
            const double wanted = (startDeg + 30.0) * kDeg2Rad;
            CHECK(dot(landed, fromAzEl(wanted, 0.0)) == doctest::Approx(1.0).epsilon(1e-9));
        }
    }

    SUBCASE("a direction ON the axis is a fixed point") {
        //  There is nothing for a spin to turn and nowhere for a slide to send it.
        const TapSettings t = plainTap({0.0, 0.0, 1.0}, 40.0 * kDeg2Rad, 0.5);
        const Vec3 after = pingStep(t, {0.0, 0.0, 1.0});
        CHECK(after.z == doctest::Approx(1.0).epsilon(1e-9));
    }

    SUBCASE("the slide is warp.hpp's own map, not a second statement of it") {
        /*  Catches: a second copy of the Mobius map in ping.cpp that disagrees with warp.hpp's. */
        const double skew = 0.4;
        const TapSettings t = plainTap({0.0, 0.0, 1.0}, 0.0, skew);
        for (const double z : {-0.8, -0.2, 0.0, 0.3, 0.9}) {
            const double r = std::sqrt(1.0 - z * z);
            const Vec3 after = pingStep(t, {r, 0.0, z});
            CHECK(after.z == doctest::Approx(slideZ(z, skew)).epsilon(1e-9));
        }
    }

    SUBCASE("a positive skew gathers toward the axis's tip") {
        const TapSettings t = plainTap({0.0, 0.0, 1.0}, 0.0, 0.5);
        CHECK(pingStep(t, {1.0, 0.0, 0.0}).z > 0.0);                                  // the equator lands above it
        CHECK(pingStep(t, {1.0, 0.0, 0.0}).y == doctest::Approx(0.0).epsilon(1e-9));  // azimuth untouched
    }

    SUBCASE("an axis that is not the pole turns about ITSELF") {
        //  Catches: the world's z used instead of the tap's axis.
        const Vec3 axis = unit({0.0, 1.0, 0.0});  // left
        const TapSettings t = plainTap(axis, 37.0 * kDeg2Rad);
        const Vec3 after = pingStep(t, {1.0, 0.0, 0.0});
        CHECK(dot(after, axis) == doctest::Approx(0.0).epsilon(1e-9));  // stays on its great circle
        CHECK(dot(after, Vec3{1.0, 0.0, 0.0}) == doctest::Approx(std::cos(37.0 * kDeg2Rad)).epsilon(1e-9));
    }
}

TEST_CASE("a spiral is a bead a pass, falling as the audio does") {
    const TapSettings t = plainTap({0.0, 0.0, 1.0}, 30.0 * kDeg2Rad, 0.2);
    const auto beads = pingSpiral(t, {1.0, 0.0, 0.0}, 6, 0.7, 0.5);
    REQUIRE(beads.size() == 6);

    CHECK(beads[0].index == 1);
    CHECK(beads[0].gain == doctest::Approx(0.7));
    CHECK(beads[1].gain == doctest::Approx(0.35));  // feedback, once
    CHECK(beads[5].gain == doctest::Approx(0.7 * std::pow(0.5, 5)));

    //  every bead is on the sphere, and each is the one before it put through the pass again
    for (std::size_t i = 0; i < beads.size(); ++i) {
        const auto& b = beads[i];
        CHECK(std::sqrt(dot(b.direction, b.direction)) == doctest::Approx(1.0).epsilon(1e-9));
        const Vec3 expected = pingStep(t, i == 0 ? Vec3{1.0, 0.0, 0.0} : beads[i - 1].direction);
        CHECK(dot(b.direction, expected) == doctest::Approx(1.0).epsilon(1e-9));
    }

    SUBCASE("blur compounds as the square root of the passes") {
        //  Catches: blur compounded linearly, or not at all.
        TapSettings blurred = t;
        blurred.blurRad = 10.0 * kDeg2Rad;
        const auto b = pingSpiral(blurred, {1.0, 0.0, 0.0}, 4);
        CHECK(b[0].blurRad == doctest::Approx(10.0 * kDeg2Rad));
        CHECK(b[3].blurRad == doctest::Approx(10.0 * kDeg2Rad * 2.0));  // sqrt(4)
    }

    SUBCASE("no passes is no spiral, and never a bead at the direction itself") {
        CHECK(pingSpiral(t, {1.0, 0.0, 0.0}, 0).empty());
    }
}

TEST_CASE("a spiral is a curve, not a chain of chords") {
    /*  Sampled in turn, so a loop draws as one continuous spiral rather than a row of chords
        between its images. */
    TapSettings t;
    t.axis = {0.0, 0.0, 1.0};
    t.spinRad = 90.0 * kDeg2Rad;  // a quarter turn a pass: four passes is a full circle
    std::vector<Vec3> pts;

    pingTrace(t, {1.0, 0.0, 0.0}, 4, pts);
    REQUIRE(pts.size() > 100);  // 360 degrees at 3 a step, not 4 points

    SUBCASE("every step is a small angle, so a circle stays a circle") {
        //  Catches: sampling in passes, where the step would be 90 degrees, not 3.
        double widest = 0.0;
        for (std::size_t i = 1; i < pts.size(); ++i)
            widest = std::max(widest, std::acos(std::clamp(dot(pts[i - 1], pts[i]), -1.0, 1.0)) * kRad2Deg);
        CHECK(widest < 2.0 * kPingStepDeg);
    }

    SUBCASE("and it lands where the passes do") {
        //  the curve passes through each whole pass's image: the beads sit ON the thread
        const auto beads = pingSpiral(t, {1.0, 0.0, 0.0}, 4);
        for (const auto& bead : beads) {
            double closest = -2.0;
            for (const auto& p : pts) closest = std::max(closest, dot(p, bead.direction));
            CHECK(closest > std::cos(kPingStepDeg * kDeg2Rad));
        }
    }

    SUBCASE("the skew composes as a Mobius map, so half a pass is half way along") {
        //  Catches: the skew interpolated linearly in passes, which makes a skewed loop bulge.
        CHECK(composeSkew(0.5, 0.5) == doctest::Approx(2.0 * 0.5 / (1.0 + 0.25)));
        TapSettings skewed = t;
        skewed.skew = 0.5;
        CHECK(loopAt(skewed, 2.0).skew == doctest::Approx(composeSkew(0.5, 0.5)));
        CHECK(loopAt(skewed, 0.0).skew == doctest::Approx(0.0));
        //  and it never comes back: every pass gathers a little more toward the same pole
        CHECK(loopAt(skewed, 3.0).skew > loopAt(skewed, 2.0).skew);
    }

    SUBCASE("a tap that never turns still traces its slide") {
        TapSettings still;
        still.axis = {0.0, 0.0, 1.0};
        still.spinRad = 0.0;
        still.skew = 0.4;
        pingTrace(still, {1.0, 0.0, 0.0}, 3, pts);
        CHECK(pts.size() >= static_cast<std::size_t>(kPingMinSteps));
        CHECK(pts.back().z > pts.front().z);  // it climbed toward the axis and never turned
    }
}

TEST_CASE("the flow is seeded on the axis's own equator") {
    //  Where the field stands before any pass has touched it -- not at the first pass's skew,
    //  which would leave a gap between the axis ring and the flow lines.
    TapSettings t;
    t.axis = unit({0.3, 0.6, 0.74});
    std::array<Vec3, kFlowSpokes> seeds{};
    flowSeeds(t, seeds);
    for (const auto& seed : seeds) {
        CHECK(std::sqrt(dot(seed, seed)) == doctest::Approx(1.0).epsilon(1e-9));
        CHECK(dot(seed, unit(t.axis)) == doctest::Approx(0.0).epsilon(1e-9));  // on its equator
    }
    //  and evenly spaced around it
    CHECK(dot(seeds[0], seeds[kFlowSpokes / 2]) == doctest::Approx(-1.0).epsilon(1e-9));
}

TEST_CASE("a traced pass has no corner at a whole pass, however high the skew") {
    /*  Skews compose as velocities do, so a fraction of a pass has its own skew and the line runs smoothly
        through each whole pass. Catches: linear blending between whole passes, which puts a corner at
        every pass. Measured as the slide's rate, which must not jump across a whole pass. */
    const TapSettings t = plainTap({0.0, 0.0, 1.0}, 0.0, 0.5);
    const Vec3 from{1.0, 0.0, 0.0};
    const auto zAt = [&](double passes) { return imageAt(t, from, loopAt(t, passes)).z; };
    constexpr double h = 1e-4;
    for (const double whole : {1.0, 2.0}) {
        const double before = (zAt(whole) - zAt(whole - h)) / h;
        const double after = (zAt(whole + h) - zAt(whole)) / h;
        INFO("pass ", whole, ": slide rate ", before, " before, ", after, " after");
        CHECK(std::abs(after - before) < 1e-2 * std::max(std::abs(before), 1e-3));
    }
    //  and it is the composed skew at a whole pass, as the spiral's beads are
    CHECK(loopAt(t, 2.0).skew == doctest::Approx(composeSkew(0.5, 0.5)).epsilon(1e-12));
}
