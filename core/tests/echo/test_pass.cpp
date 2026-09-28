// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstring>
#include <vector>

#include "bambi/dsp/field.hpp"
#include "bambi/echo/pass.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kFs = 48000.0;

std::vector<float> noiseField(int order, int frames, unsigned seed = 7u) {
    std::vector<float> f(static_cast<std::size_t>(numChannels(order) * frames));
    unsigned s = seed;
    for (float& v : f) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(s >> 8) % 20001 - 10000) / 10000.0f;
    }
    return f;
}

struct Rig {
    int order;
    PassWork work;
    TapTransform tap;
    BandState band;
    explicit Rig(int n, int maxFrames = 128) : order(n) {
        work.prepare(n, maxFrames);
        prepareTap(tap, n);
        band.prepare(n);
    }
    void set(const TapSettings& s) { buildTap(s, kFs, work, tap); }
    void openBand() { tap.lowpass = 1.0, tap.highpass = 0.0; }  // a band that is not there, for the geometry tests
};

TapSettings tilted() {
    TapSettings s;
    s.axis = unit(Vec3{0.4, -0.7, 0.5});
    s.axisIsPole = false;
    s.skew = 0.45;
    s.spinRad = 0.6;
    s.blurRad = 20.0 * kDeg2Rad;
    return s;
}

}  // namespace

TEST_CASE("a pass is its stages in order, as the double reference computes them") {
    //  Turn to the pole, skew, spin, turn back, blur -- each from its own tested piece, in double,
    //  against the float loop. Catches: a stage out of order, the two turns swapped, the blur dropped.
    for (const int order : {1, 3, 7}) {
        Rig rig(order);
        const TapSettings s = tilted();
        rig.set(s);
        rig.openBand();
        const int ch = numChannels(order), frames = 3;
        auto data = noiseField(order, frames);
        const auto in = data;
        onePass(rig.tap, rig.band, rig.work, data.data(), frames);

        std::vector<double> toPole(rotationSize(order)), back(rotationSize(order)), spin(rotationSize(order));
        const Mat3 r = axisToPole(s.axis);
        shRotation(r, order, toPole);
        shRotation(transposed(r), order, back);
        shRotation(rotationAbout({0, 0, 1}, s.spinRad), order, spin);
        WarpBuilder wb;
        wb.prepare(order);
        std::vector<double> warp(warpSize(order)), caps(order + 1);
        wb.build(s.skew, warp);
        capWeights(s.blurRad, order, caps);

        std::vector<double> a(ch), b(ch);
        double worst = 0.0;
        for (int f = 0; f < frames; ++f) {
            for (int c = 0; c < ch; ++c) a[c] = in[static_cast<std::size_t>(f * ch + c)];
            rotateSH(toPole, order, a, b);
            applyWarp(warp, order, b, a);
            rotateSH(spin, order, a, b);
            rotateSH(back, order, b, a);
            for (int c = 0; c < ch; ++c)
                worst =
                    std::max(worst, std::abs(a[c] * caps[acnOrder(c)] - data[static_cast<std::size_t>(f * ch + c)]));
        }
        INFO("order ", order, " worst ", worst);
        CHECK(worst < 2e-5);
    }
}

TEST_CASE("any rotation that carries the axis to the pole gives the same pass") {
    /*  axisToPole is one of many: any turn about the pole may follow it. The pass is turned there and
        back on the strength of the skew and the spin both commuting with such a turn, and this is
        that, tested -- with it, a different but equally valid construction changes nothing. */
    const int order = 5, frames = 2;
    Rig rig(order);
    const TapSettings s = tilted();
    rig.set(s);
    rig.openBand();
    auto first = noiseField(order, frames);
    auto second = first;
    onePass(rig.tap, rig.band, rig.work, first.data(), frames);

    std::vector<double> blocks(rotationSize(order));
    shRotation(multiply(rotationAbout({0, 0, 1}, 1.1), axisToPole(s.axis)), order, blocks);
    fieldBlocks(blocks, order, rig.tap.toPole, rig.tap.fromPole);
    onePass(rig.tap, rig.band, rig.work, second.data(), frames);
    for (std::size_t k = 0; k < first.size(); ++k) CHECK(std::abs(first[k] - second[k]) < 2e-5);
}

TEST_CASE("an axis that is the pole needs no turns, and says so rather than being found out") {
    const int order = 5, frames = 2;
    TapSettings s = tilted();
    s.axis = {0, 0, 1};
    Rig skipped(order), turned(order);
    s.axisIsPole = true;
    skipped.set(s);
    s.axisIsPole = false;  // the same axis, taken the long way round
    turned.set(s);
    skipped.openBand();
    turned.openBand();
    CHECK(skipped.tap.plain);
    CHECK_FALSE(turned.tap.plain);
    auto a = noiseField(order, frames), b = a;
    onePass(skipped.tap, skipped.band, skipped.work, a.data(), frames);
    onePass(turned.tap, turned.band, turned.work, b.data(), frames);
    for (std::size_t k = 0; k < a.size(); ++k) CHECK(std::abs(a[k] - b[k]) < 2e-5);
}

TEST_CASE("the band never raises a level, passes nothing at DC, and compounds with each pass") {
    Rig rig(0);
    TapSettings s;
    s.lowCutHz = 200.0;
    s.highCutHz = 4000.0;
    rig.set(s);
    const auto gainAt = [&](double hz) {
        rig.band.clear();
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i) {
            float x = static_cast<float>(std::sin(2.0 * kPi * hz * i / kFs));
            onePass(rig.tap, rig.band, rig.work, &x, 1);
            if (i > 24000) peak = std::max(peak, static_cast<double>(std::abs(x)));
        }
        return peak;
    };
    for (const double hz : {30.0, 200.0, 900.0, 4000.0, 15000.0}) CHECK(gainAt(hz) <= 1.0);
    CHECK(gainAt(900.0) > 0.7);    // inside the band
    CHECK(gainAt(30.0) < 0.2);     // below it
    CHECK(gainAt(15000.0) < 0.4);  // above it

    //  and it is the low-pass minus a low-pass of it, read off the first sample of an impulse, where
    //  the two coefficients appear alone
    rig.band.clear();
    float hit = 1.0f;
    onePass(rig.tap, rig.band, rig.work, &hit, 1);
    CHECK(static_cast<double>(hit) == doctest::Approx(rig.tap.lowpass * (1.0 - rig.tap.highpass)).epsilon(1e-6));

    rig.band.clear();
    float dc = 1.0f;
    for (int i = 0; i < 48000; ++i) {
        dc = 1.0f;
        onePass(rig.tap, rig.band, rig.work, &dc, 1);
    }
    CHECK(std::abs(dc) < 1e-4f);  // the stability of a loop near unity rests on this
}

TEST_CASE("blur compounds as the root of the passes") {
    /*  A loop applies the same gain per order every pass, so after j passes a blur of a is the cap
        weights to the j, which is approximately a blur of a * sqrt(j). The ping draws blur with
        that law, so this checks the drawing against the audio. */
    std::vector<double> once(4), wide(4);
    capWeights(6.0 * kDeg2Rad, 3, once);
    for (const int j : {4, 9, 16}) {
        capWeights(6.0 * kDeg2Rad * std::sqrt(static_cast<double>(j)), 3, wide);
        //  exact as the blur goes to nothing; at 6 degrees and 16 passes the two differ by 0.009
        for (int n = 0; n <= 3; ++n) CHECK(std::abs(std::pow(once[n], j) - wide[n]) < 0.01);
    }
    //  and under a quarter of a degree there is no blur at all
    Rig rig(3);
    TapSettings s;
    s.blurRad = 0.1 * kDeg2Rad;
    rig.set(s);
    for (const float g : rig.tap.gains) CHECK(g == 1.0f);
}

TEST_CASE("a pass gives the same bits for one call of many frames as for many calls of one, band included") {
    const int order = 7, frames = 97, ch = numChannels(order);
    TapSettings s = tilted();
    s.lowCutHz = 120.0;
    s.highCutHz = 6000.0;
    const auto in = noiseField(order, frames);
    const auto run = [&](const std::vector<int>& pieces) {
        Rig rig(order);
        rig.set(s);
        auto data = in;
        int at = 0;
        for (const int n : pieces) {
            onePass(rig.tap, rig.band, rig.work, data.data() + at * ch, n);
            at += n;
        }
        return data;
    };
    const auto whole = run({frames}), ones = run(std::vector<int>(frames, 1)), uneven = run({1, 31, 2, 63});
    CHECK(std::memcmp(whole.data(), ones.data(), whole.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(whole.data(), uneven.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE("a band state decaying into silence becomes exactly silent") {
    //  By a compare, so it is the same on every machine and needs no processor flag. Catches: the
    //  flush removed, leaving states at 1e-30 and smaller for as long as the tap lives.
    Rig rig(1);
    TapSettings s;
    s.lowCutHz = 40.0;
    s.highCutHz = 300.0;
    rig.set(s);
    std::vector<float> frame(4, 1.0f);
    onePass(rig.tap, rig.band, rig.work, frame.data(), 1);
    for (int i = 0; i < 48000 * 4; ++i) {
        std::fill(frame.begin(), frame.end(), 0.0f);
        onePass(rig.tap, rig.band, rig.work, frame.data(), 1);
    }
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(rig.band.low[c] == 0.0);
        CHECK(rig.band.high[c] == 0.0);
    }
}
