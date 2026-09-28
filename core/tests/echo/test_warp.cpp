// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "bambi/echo/warp.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

std::vector<double> warp(int order, double skew, int nodes = kWarpNodes) {
    WarpBuilder b;
    b.prepare(order, nodes);
    std::vector<double> blocks(static_cast<std::size_t>(warpSize(order)));
    b.build(skew, blocks);
    return blocks;
}

//  Where the map sends a direction: along its meridian, azimuth untouched.
Vec3 image(Vec3 d, double a) {
    const double z = (d.z + a) / (1.0 + a * d.z);
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z)), az = std::atan2(d.y, d.x);
    return {r * std::cos(az), r * std::sin(az), z};
}

double entry(const std::vector<double>& blocks, int order, int m, int ni, int nj) {
    const int w = order - m + 1;
    return blocks[static_cast<std::size_t>(warpOffset(order, m) + w * (ni - m) + (nj - m))];
}

}  // namespace

TEST_CASE("no skew is no change, at every order") {
    //  Catches: a wrong azimuth factor -- doubled, every entry of the diagonal is 2.
    for (const int order : {1, 3, 5, 7}) {
        const auto b = warp(order, 0.0);
        double worst = 0.0;
        for (int m = 0; m <= order; ++m)
            for (int ni = m; ni <= order; ++ni)
                for (int nj = m; nj <= order; ++nj)
                    worst = std::max(worst, std::abs(entry(b, order, m, ni, nj) - (ni == nj ? 1.0 : 0.0)));
        CHECK(worst < 1e-12);
    }
    CHECK(warpSize(3) == 30);
    CHECK(warpSize(7) == 204);
    CHECK(warpOffset(7, 1) == 64);
}

TEST_CASE("a source at d becomes a source at T(d), at unit gain") {
    /*  The definition: the matrix applied to the encoding of d is the encoding of T(d). It
        holds exactly only with every order present, so it is built at order 30 and read in its first
        16 channels, where what truncation leaves is below 1e-9 at a skew of 0.6. This is the test
        that tells the column factor from the row one -- on the row it fails by 0.1 to 1.3 -- and a
        matrix that carried the Jacobian from one that does not. The equator landing at arccos(a)
        is this, at d on the equator. */
    const int top = 30, low = 3;
    std::vector<double> y(numChannels(top)), moved(numChannels(top)), want(numChannels(low));
    for (const double a : {0.3, -0.3, 0.6}) {
        const auto b = warp(top, a, 256);
        double worst = 0.0;
        for (const Vec3& d : {fromAzEl(0.4, 0.0), fromAzEl(-2.0, 0.7), fromAzEl(1.1, -0.9), fromAzEl(3.0, 0.2)}) {
            shSN3D(d, top, y);
            applyWarp(b, top, y, moved);
            shSN3D(image(d, a), low, want);
            for (std::size_t c = 0; c < want.size(); ++c) worst = std::max(worst, std::abs(moved[c] - want[c]));
        }
        INFO("skew ", a, " worst ", worst);
        CHECK(worst < 1e-8);
    }
    //  the equator, where the map is easiest to say: it lands at arccos(a) from the pole
    CHECK(std::abs(std::acos(image(fromAzEl(1.0, 0.0), 0.5).z) - std::acos(0.5)) < 1e-12);
}

TEST_CASE("the omni row is untouched: what comes out has the same total as what went in") {
    //  Exact at any order, and the mark of the matrix WITHOUT the Jacobian; with it, it is the omni
    //  column that is untouched instead.
    for (const int order : {3, 7})
        for (const double a : {0.3, -0.6, 0.9}) {
            const auto b = warp(order, a);
            for (int nj = 0; nj <= order; ++nj)
                CHECK(std::abs(entry(b, order, 0, 0, nj) - (nj == 0 ? 1.0 : 0.0)) < 1e-12);
        }
}

TEST_CASE("a negative skew is the positive one seen in a mirror, and one undoes the other") {
    const int order = 7;
    const auto plus = warp(order, 0.4), minus = warp(order, -0.4);
    for (int m = 0; m <= order; ++m)
        for (int ni = m; ni <= order; ++ni)
            for (int nj = m; nj <= order; ++nj) {
                const double sign = ((ni + nj) % 2 == 0) ? 1.0 : -1.0;  // z -> -z flips odd n + m
                CHECK(std::abs(entry(minus, order, m, ni, nj) - sign * entry(plus, order, m, ni, nj)) < 1e-12);
            }

    //  W(a) W(-a) = I, and W(0.3) W(0.4) = W of the two composed, on the low block of a high build
    const int top = 30;
    const auto a = warp(top, 0.3, 256), back = warp(top, -0.3, 256), c = warp(top, 0.4, 256);
    const auto both = warp(top, (0.3 + 0.4) / (1.0 + 0.3 * 0.4), 256);
    std::vector<double> y(numChannels(top)), mid(numChannels(top)), out(numChannels(top)), once(numChannels(top));
    shSN3D(fromAzEl(0.7, 0.3), top, y);
    applyWarp(back, top, y, mid);
    applyWarp(a, top, mid, out);
    for (int ch = 0; ch < 16; ++ch)
        CHECK(std::abs(out[static_cast<std::size_t>(ch)] - y[static_cast<std::size_t>(ch)]) < 1e-8);
    applyWarp(c, top, y, mid);
    applyWarp(a, top, mid, out);
    applyWarp(both, top, y, once);
    for (int ch = 0; ch < 16; ++ch)
        CHECK(std::abs(out[static_cast<std::size_t>(ch)] - once[static_cast<std::size_t>(ch)]) < 1e-8);
}

TEST_CASE("64 nodes are enough at the largest skew, where a grid sized for polynomials is not") {
    //  The integrand is rational, with a pole at -1/a. On the 2N+4 nodes that are exact for a
    //  polynomial, the entries at a skew of 0.9 are off by 0.023 at order 3 and 0.056 at order 7; on
    //  64 they agree with 256 to 3e-15. Catches: the node count lowered, and the skew's clamp removed.
    for (const int order : {3, 7}) {
        const auto b64 = warp(order, 0.9), b256 = warp(order, 0.9, 256), coarse = warp(order, 0.9, 2 * order + 4);
        double fine = 0.0, rough = 0.0;
        for (std::size_t k = 0; k < b64.size(); ++k) {
            fine = std::max(fine, std::abs(b64[k] - b256[k]));
            rough = std::max(rough, std::abs(coarse[k] - b256[k]));
        }
        CHECK(fine < 1e-11);
        CHECK(rough > 0.01);
    }
    const auto clamped = warp(3, 5.0), most = warp(3, kMaxSkew);
    for (std::size_t k = 0; k < most.size(); ++k) CHECK(clamped[k] == most[k]);
}
