// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

/*  These are not arithmetic tests. They guard the coordinate convention, so that anyone who
 *  "corrects" it to a graphics or a FuMa convention breaks the build immediately rather than
 *  three modules downstream.
 */
TEST_CASE("convention: x front, y left, z up; azimuth CCW from front") {
    //  Catches: azimuth turned clockwise, so left is -y.
    const double d = kDeg2Rad;

    SUBCASE("cardinal directions") {
        const Vec3 front = fromAzEl(0 * d, 0 * d);
        CHECK(front.x == doctest::Approx(1.0));
        CHECK(front.y == doctest::Approx(0.0));
        CHECK(front.z == doctest::Approx(0.0));

        const Vec3 left = fromAzEl(90 * d, 0 * d);  // azimuth is COUNTER-clockwise
        CHECK(left.x == doctest::Approx(0.0));
        CHECK(left.y == doctest::Approx(1.0));
        CHECK(left.z == doctest::Approx(0.0));

        const Vec3 up = fromAzEl(0 * d, 90 * d);  // elevation is positive UP
        CHECK(up.z == doctest::Approx(1.0));

        const Vec3 right = fromAzEl(-90 * d, 0 * d);
        CHECK(right.y == doctest::Approx(-1.0));
    }

    SUBCASE("round trip") {
        for (int i = 0; i < 10; ++i) {  // -170 to 163 degrees in azimuth
            const double az = -170.0 + 37.0 * i;
            for (int j = 0; j < 7; ++j) {  // -80 to 58 in elevation
                const double el = -80.0 + 23.0 * j;
                const Vec3 p = fromAzEl(az * d, el * d);
                CHECK(azimuth(p) * kRad2Deg == doctest::Approx(az).epsilon(1e-9));
                CHECK(elevation(p) * kRad2Deg == doctest::Approx(el).epsilon(1e-9));
                CHECK(length(p) == doctest::Approx(1.0));
            }
        }
    }

    SUBCASE("right-handed: front cross left is up") {
        const Vec3 c = cross(fromAzEl(0, 0), fromAzEl(90 * kDeg2Rad, 0));
        CHECK(c.z == doctest::Approx(1.0));
    }

    SUBCASE("arc") {
        CHECK(arc(fromAzEl(0, 0), fromAzEl(0, 0)) == doctest::Approx(0.0));
        CHECK(arc(fromAzEl(0, 0), fromAzEl(90 * kDeg2Rad, 0)) == doctest::Approx(kPi / 2));
        CHECK(arc(fromAzEl(0, 0), fromAzEl(kPi, 0)) == doctest::Approx(kPi));
    }
}
