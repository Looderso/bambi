// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr int kTop = 10;

std::vector<Vec3> directions(int n = 60) {
    std::vector<Vec3> out;
    const double ga = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt(std::max(0.0, 1.0 - z * z));
        out.push_back({r * std::cos(i * ga), r * std::sin(i * ga), z});
    }
    return out;
}

std::vector<Mat3> rotations() {
    return {rotationAbout({0, 0, 1}, 0.7),
            rotationAbout({0, 1, 0}, -1.9),
            rotationAbout({1, 0, 0}, 2.6),
            rotationAbout({0.3, -1.0, 0.6}, 1.234),
            rotationAbout({-0.8, 0.1, -0.5}, 3.0),
            multiply(rotationAbout({0, 0, 1}, 2.0), rotationAbout({0, 1, 0}, kPi / 2))};
}

std::vector<double> blocksFor(const Mat3& r, int order) {
    std::vector<double> b(static_cast<std::size_t>(rotationSize(order)));
    shRotation(r, order, b);
    return b;
}

}  // namespace

TEST_CASE("the blocks do to a field what the rotation does to space") {
    /*  The definition, and the oracle: a source at d, turned, is a source at R*d -- so the blocks
        applied to the encoding of d must be the encoding of R*d, in every channel, to order 10. This
        is what pins the convention: a transposed block, or the first order's axes taken as x, y, z
        instead of the channel order y, z, x, each fail it at once. */
    std::vector<double> y(numChannels(kTop)), turned(numChannels(kTop)), want(numChannels(kTop));
    double worst = 0.0;
    for (const Mat3& r : rotations()) {
        const auto blocks = blocksFor(r, kTop);
        for (const Vec3& d : directions()) {
            shSN3D(d, kTop, y);
            rotateSH(blocks, kTop, y, turned);
            shSN3D(apply(r, d), kTop, want);
            for (std::size_t c = 0; c < y.size(); ++c) worst = std::max(worst, std::abs(turned[c] - want[c]));
        }
    }
    INFO("worst channel error ", worst);
    CHECK(worst < 1e-11);
}

TEST_CASE("a source at 35 degrees, turned 90 about up, is at 125 -- not at -55") {
    //  Catches: a rotation applied backwards, which turns the source the wrong way round.
    const auto blocks = blocksFor(rotationAbout({0, 0, 1}, kPi / 2), 3);
    std::vector<double> y(16), turned(16), left(16), right(16);
    shSN3D(fromAzEl(35.0 * kDeg2Rad, 0.0), 3, y);
    rotateSH(blocks, 3, y, turned);
    shSN3D(fromAzEl(125.0 * kDeg2Rad, 0.0), 3, left);
    shSN3D(fromAzEl(-55.0 * kDeg2Rad, 0.0), 3, right);
    double toLeft = 0.0, toRight = 0.0;
    for (std::size_t c = 0; c < 16; ++c) {
        toLeft = std::max(toLeft, std::abs(turned[c] - left[c]));
        toRight = std::max(toRight, std::abs(turned[c] - right[c]));
    }
    CHECK(toLeft < 1e-12);
    CHECK(toRight > 0.5);
}

TEST_CASE("each block is orthogonal, and two turns are the blocks of their product") {
    const auto all = rotations();
    for (const Mat3& r : all) {
        const auto b = blocksFor(r, kTop);
        for (int n = 0; n <= kTop; ++n) {
            const int w = 2 * n + 1;
            const double* m = b.data() + rotationOffset(n);
            for (int i = 0; i < w; ++i)
                for (int j = 0; j < w; ++j) {
                    double dot = 0.0;
                    for (int k = 0; k < w; ++k) dot += m[w * i + k] * m[w * j + k];
                    REQUIRE(std::abs(dot - (i == j ? 1.0 : 0.0)) < 1e-11);
                }
        }
    }
    //  the blocks of a after b, against the blocks of a applied after the blocks of b
    const Mat3 a = all[3], c = all[4];
    const auto ba = blocksFor(a, 7), bc = blocksFor(c, 7), both = blocksFor(multiply(a, c), 7);
    std::vector<double> y(64), mid(64), twice(64), once(64);
    shSN3D(unit(Vec3{0.2, -0.7, 0.4}), 7, y);
    rotateSH(bc, 7, y, mid);
    rotateSH(ba, 7, mid, twice);
    rotateSH(both, 7, y, once);
    for (std::size_t k = 0; k < 64; ++k) CHECK(std::abs(twice[k] - once[k]) < 1e-12);
}

TEST_CASE("no turn is the identity, and a block does not depend on how many orders were asked for") {
    const auto none = blocksFor(kIdentity3, 5);
    for (int n = 0; n <= 5; ++n) {
        const int w = 2 * n + 1;
        for (int i = 0; i < w; ++i)
            for (int j = 0; j < w; ++j)
                CHECK(none[static_cast<std::size_t>(rotationOffset(n) + w * i + j)] == (i == j ? 1.0 : 0.0));
    }
    const Mat3 r = rotations()[3];
    const auto low = blocksFor(r, 3), high = blocksFor(r, 9);
    for (std::size_t k = 0; k < low.size(); ++k) CHECK(low[k] == high[k]);

    CHECK(rotationSize(0) == 1);
    CHECK(rotationSize(1) == 10);
    CHECK(rotationSize(7) == 680);  // against 64 * 64 = 4096 for a matrix that knew nothing
    CHECK(rotationOffset(2) == 10);
}

TEST_CASE("axisToPole carries its axis to the pole, from anywhere, and is a rotation") {
    for (const Vec3& a : directions(40)) {
        const Mat3 r = axisToPole(a);
        CHECK(arc(apply(r, a), {0, 0, 1}) < 1e-7);
        const Mat3 rrT = multiply(r, transposed(r));
        for (std::size_t k = 0; k < 9; ++k) CHECK(std::abs(rrT[k] - kIdentity3[k]) < 1e-12);
    }
    const Mat3 down = axisToPole({0, 0, -1});
    CHECK(arc(apply(down, {0, 0, -1}), {0, 0, 1}) < 1e-12);
    //  a rotation, not the point inversion that carries it there too: it keeps a cross product
    const Vec3 x = apply(down, {1, 0, 0}), y = apply(down, {0, 1, 0}), z = apply(down, {0, 0, 1});
    CHECK(length(cross(x, y) - z) < 1e-12);
    const Mat3 same = axisToPole({0, 0, 1});
    for (std::size_t k = 0; k < 9; ++k) CHECK(same[k] == kIdentity3[k]);
}
