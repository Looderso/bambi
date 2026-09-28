// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

/*  A sphere quadrature that integrates the product of any two harmonics of order <= `order`
 *  EXACTLY: order+1 Gauss-Legendre nodes in z, 2*order+2 equispaced azimuths. The azimuth
 *  rule is exact for the trigonometric products (frequency <= 2*order), which makes every pair
 *  with different |m| vanish identically; for equal |m| the z-integrand is a polynomial of
 *  degree <= 2*order, which Gauss-Legendre handles exactly.
 *
 *  An approximate quadrature, such as a large Fibonacci-point average, would turn a test at
 *  high order into a test of the integration rather than of the harmonics. */
struct Quadrature {
    std::vector<Vec3> points;
    std::vector<double> weights;  ///< sum to 1: a weighted sum is a mean over the sphere
};

Quadrature exactQuadrature(int order) {
    std::vector<double> z, wz;
    gaussLegendre(order + 1, z, wz);
    const int nPhi = 2 * order + 2;
    Quadrature q;
    for (std::size_t i = 0; i < z.size(); ++i) {
        const double r = std::sqrt(std::max(0.0, 1.0 - z[i] * z[i]));
        for (int j = 0; j < nPhi; ++j) {
            const double phi = 2.0 * kPi * j / nPhi;
            q.points.push_back({r * std::cos(phi), r * std::sin(phi), z[i]});
            q.weights.push_back(wz[i] / (2.0 * nPhi));
        }
    }
    return q;
}

}  // namespace

TEST_CASE("acn indexing") {
    CHECK(numChannels(0) == 1);
    CHECK(numChannels(3) == 16);
    CHECK(numChannels(7) == 64);
    for (int n = 0; n <= kMaxOrder; ++n) {
        for (int m = -n; m <= n; ++m) {
            const int acn = n * n + n + m;
            CHECK(acnOrder(acn) == n);
            CHECK(acnDegree(acn) == m);
        }
    }
}

/*  The definitive check: for SN3D the mean of Y^2 over the sphere is exactly 1/(2n+1),
 *  so mean * (2n+1) == 1 for every channel. This pins the normalisation and nothing else —
 *  note that it is blind to sign errors, which is what the agreement test below is for.
 */
TEST_CASE("SN3D normalisation is exact on every channel, up to the ceiling") {
    const Quadrature q = exactQuadrature(kMaxOrder);
    std::vector<double> y(static_cast<std::size_t>(kMaxChannels)), acc(y.size(), 0.0);
    for (std::size_t i = 0; i < q.points.size(); ++i) {
        shSN3D(q.points[i], kMaxOrder, y);
        for (std::size_t c = 0; c < y.size(); ++c) acc[c] += q.weights[i] * y[c] * y[c];
    }
    double worst = 0.0;
    int worstChannel = -1;
    for (int c = 0; c < kMaxChannels; ++c) {
        const double err = std::abs(acc[static_cast<std::size_t>(c)] * (2.0 * acnOrder(c) + 1.0) - 1.0);
        if (err > worst) {
            worst = err;
            worstChannel = c;
        }
    }
    INFO("worst normalisation error ", worst, " on channel ", worstChannel, " (order ", acnOrder(worstChannel),
         ", degree ", acnDegree(worstChannel), ")");
    CHECK(worst < 1e-10);
}

/*  Normalisation alone is blind to two harmonics leaking into each other. Every pair, to 10th
 *  order -- the highest order a REAPER track can carry (121 of its 128 channels). */
TEST_CASE("SN3D channels are mutually orthogonal, every pair, to 10th order") {
    constexpr int kOrder = 10;
    const int ch = numChannels(kOrder);
    const Quadrature q = exactQuadrature(kOrder);
    std::vector<double> y(static_cast<std::size_t>(ch)), gram(static_cast<std::size_t>(ch * ch), 0.0);
    for (std::size_t i = 0; i < q.points.size(); ++i) {
        shSN3D(q.points[i], kOrder, y);
        for (int a = 0; a < ch; ++a)
            for (int b = 0; b < ch; ++b)
                gram[static_cast<std::size_t>(a * ch + b)] +=
                    q.weights[i] * y[static_cast<std::size_t>(a)] * y[static_cast<std::size_t>(b)];
    }
    double worst = 0.0;
    for (int a = 0; a < ch; ++a)
        for (int b = 0; b < ch; ++b) {
            const double want = a == b ? 1.0 / (2.0 * acnOrder(a) + 1.0) : 0.0;
            worst = std::max(worst, std::abs(gram[static_cast<std::size_t>(a * ch + b)] - want));
        }
    INFO("worst Gram error ", worst);
    CHECK(worst < 1e-13);
}

TEST_CASE("cardinal directions land on the expected channels") {
    std::array<double, kMaxChannels> y{};

    shSN3D(fromAzEl(0, 0), 3, y);  // front -> +x -> ACN 3
    CHECK(y[0] == doctest::Approx(1.0));
    CHECK(y[1] == doctest::Approx(0.0));
    CHECK(y[2] == doctest::Approx(0.0));
    CHECK(y[3] == doctest::Approx(1.0));

    shSN3D(fromAzEl(90 * kDeg2Rad, 0), 3, y);  // left -> +y -> ACN 1
    CHECK(y[1] == doctest::Approx(1.0));
    CHECK(y[2] == doctest::Approx(0.0));
    CHECK(y[3] == doctest::Approx(0.0));

    shSN3D(fromAzEl(0, 90 * kDeg2Rad), 3, y);  // up -> +z -> ACN 2
    CHECK(y[1] == doctest::Approx(0.0));
    CHECK(y[2] == doctest::Approx(1.0));
    CHECK(y[3] == doctest::Approx(0.0));
}

/*  The headline test. Two independent implementations — a general recurrence and hardcoded
 *  Cartesian closed forms — must agree to 1e-12. This is what catches the Condon-Shortley
 *  sign error, which flips odd-degree channels while leaving every magnitude, and therefore
 *  every normalisation check, perfectly intact.
 */
TEST_CASE("recursive implementation agrees with the closed-form oracle to 1e-12") {
    std::mt19937 rng(20260910);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);

    std::array<double, kMaxChannels> a{}, b{};
    double worst = 0.0;
    int worstChannel = -1;

    for (int i = 0; i < 1000; ++i) {
        const Vec3 p = unit({uni(rng), uni(rng), uni(rng)});
        shSN3D(p, 3, a);
        shSN3D_ref(p, 3, b);
        for (int c = 0; c < 16; ++c) {
            const double d = std::abs(a[c] - b[c]);
            if (d > worst) {
                worst = d;
                worstChannel = c;
            }
        }
    }
    INFO("worst disagreement ", worst, " on channel ", worstChannel);
    CHECK(worst < 1e-12);
}

TEST_CASE("order argument limits how many channels are written") {
    std::array<double, kMaxChannels> y{};
    y.fill(-999.0);
    shSN3D(fromAzEl(0.3, 0.2), 1, y);
    CHECK(y[3] != -999.0);
    CHECK(y[4] == -999.0);  // untouched beyond numChannels(1)
}
