// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>

#include "bambi/math/legendre.hpp"
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("legendreP: known values") {
    for (int n = 0; n <= 7; ++n) {
        CHECK(legendreP(n, 1.0) == doctest::Approx(1.0));
        CHECK(legendreP(n, -1.0) == doctest::Approx(n % 2 == 0 ? 1.0 : -1.0));
    }
    CHECK(legendreP(0, 0.0) == doctest::Approx(1.0));
    CHECK(legendreP(1, 0.0) == doctest::Approx(0.0));
    CHECK(legendreP(2, 0.0) == doctest::Approx(-0.5));
    CHECK(legendreP(3, 0.0) == doctest::Approx(0.0));
    CHECK(legendreP(4, 0.0) == doctest::Approx(0.375));
    CHECK(legendreP(2, 0.5) == doctest::Approx(-0.125));
}

TEST_CASE("legendreAssoc: agrees with legendreP at m = 0") {
    for (int n = 0; n <= 7; ++n)
        for (double x = -0.9; x <= 0.9; x += 0.3) CHECK(legendreAssoc(n, 0, x) == doctest::Approx(legendreP(n, x)));
}

TEST_CASE("capWeights: correct at both limits") {
    std::array<double, 8> w{};

    SUBCASE("alpha 0 leaves the encoding untouched") {
        capWeights(0.0, 7, w);
        for (int n = 0; n <= 7; ++n) CHECK(w[n] == doctest::Approx(1.0));
    }

    SUBCASE("alpha 0 is a limit, not a special case: 0.5 deg is already ~1") {
        capWeights(0.5 * kDeg2Rad, 3, w);
        for (int n = 0; n <= 3; ++n) CHECK(w[n] == doctest::Approx(1.0).epsilon(1e-3));
    }

    SUBCASE("alpha pi is pure omni") {
        capWeights(kPi, 7, w);
        CHECK(w[0] == doctest::Approx(1.0));
        for (int n = 1; n <= 7; ++n) CHECK(w[n] == doctest::Approx(0.0).epsilon(1e-9));
    }

    SUBCASE("hemisphere") {
        capWeights(kPi / 2, 3, w);
        CHECK(w[0] == doctest::Approx(1.0));
        CHECK(w[1] == doctest::Approx(0.5));
        CHECK(w[2] == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(w[3] == doctest::Approx(-0.125));
    }

    SUBCASE("monotonically non-increasing in alpha, per order") {
        std::array<double, 8> a{}, b{};
        for (double deg = 5; deg < 175; deg += 5) {
            capWeights(deg * kDeg2Rad, 5, a);
            capWeights((deg + 5) * kDeg2Rad, 5, b);
            CHECK(b[1] <= a[1] + 1e-12);
        }
    }
}

TEST_CASE("maxRE: matches the standard order-3 weighting") {
    std::array<double, 8> w{};
    maxRE(3, w);
    CHECK(w[0] == doctest::Approx(1.0));
    CHECK(w[1] == doctest::Approx(0.861).epsilon(2e-3));
    CHECK(w[2] == doctest::Approx(0.612).epsilon(2e-3));
    CHECK(w[3] == doctest::Approx(0.304).epsilon(2e-3));
}

TEST_CASE("Gauss-Legendre integrates polynomials exactly to degree 2n-1") {
    for (int n : {1, 4, 11, 36}) {
        std::vector<double> x, w;
        gaussLegendre(n, x, w);
        REQUIRE(x.size() == static_cast<std::size_t>(n));
        for (int k = 0; k <= 2 * n - 1; ++k) {
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
                sum += w[static_cast<std::size_t>(i)] * std::pow(x[static_cast<std::size_t>(i)], k);
            const double exact = (k % 2 == 1) ? 0.0 : 2.0 / (k + 1);
            INFO("n ", n, " degree ", k);
            CHECK(std::abs(sum - exact) < 1e-13);
        }
    }
}
