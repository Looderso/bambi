// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "bambi/encode/encoder.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
std::vector<float*> ptrsOf(std::vector<std::vector<float>>& v) {
    std::vector<float*> p;
    for (auto& c : v) p.push_back(c.data());
    return p;
}
}  // namespace

TEST_CASE("encoder channel counts follow the order") {
    CHECK(Encoder::numChannels(0) == 1);
    CHECK(Encoder::numChannels(1) == 4);
    CHECK(Encoder::numChannels(3) == 16);
    CHECK(Encoder::numChannels(7) == 64);
}

TEST_CASE("a point source lands where the harmonics say it should") {
    Encoder e;
    e.prepare(3);
    e.setTarget(fromAzEl(0, 0), 0.0);  // front
    e.snap();
    CHECK(e.gains()[0] == doctest::Approx(1.0));
    CHECK(e.gains()[1] == doctest::Approx(0.0));  // y
    CHECK(e.gains()[2] == doctest::Approx(0.0));  // z
    CHECK(e.gains()[3] == doctest::Approx(1.0));  // x

    e.setTarget(fromAzEl(90 * kDeg2Rad, 0), 0.0);  // left
    e.snap();
    CHECK(e.gains()[1] == doctest::Approx(1.0));
    CHECK(e.gains()[3] == doctest::Approx(0.0));
}

TEST_CASE("full width collapses to omni") {
    Encoder e;
    e.prepare(3);
    e.setTarget(fromAzEl(1.0, 0.3), kPi);
    e.snap();
    CHECK(e.gains()[0] == doctest::Approx(1.0));
    for (int c = 1; c < 16; ++c) {
        INFO("channel ", c);
        CHECK(std::abs(e.gains()[c]) < 1e-9);
    }
}

TEST_CASE("zero width leaves the encoding untouched") {
    Encoder a, b;
    a.prepare(3);
    b.prepare(3);
    a.setTarget(fromAzEl(0.7, 0.2), 0.0);
    b.setTarget(fromAzEl(0.7, 0.2), 1e-9);
    a.snap();
    b.snap();
    for (int c = 0; c < 16; ++c) CHECK(a.gains()[c] == doctest::Approx(b.gains()[c]));
}

/*  The reason gains ramp rather than step. A source moving fast enough to be interesting
 *  covers several degrees per block, and a stepped gain clicks at exactly the moment the
 *  plugin exists to create. */
TEST_CASE("gains interpolate across a block instead of stepping") {
    Encoder e;
    e.prepare(1);
    e.setTarget(fromAzEl(0, 0), 0.0);
    e.snap();

    const int n = 64;
    std::vector<float> in(static_cast<std::size_t>(n), 1.0f);
    std::vector<std::vector<float>> out(4, std::vector<float>(static_cast<std::size_t>(n), 0.0f));

    e.setTarget(fromAzEl(kPi, 0), 0.0);  // front to back: channel 3 goes +1 to -1
    auto p = ptrsOf(out);
    e.process(in.data(), p, n);

    CHECK(out[3][0] == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(out[3][n - 1] == doctest::Approx(-1.0f).epsilon(0.05));

    double biggestStep = 0.0;
    for (int i = 1; i < n; ++i)
        biggestStep = std::max(biggestStep, static_cast<double>(std::abs(out[3][i] - out[3][i - 1])));
    INFO("largest sample-to-sample jump ", biggestStep);
    CHECK(biggestStep < 2.5 / n * 1.5);  // a step would be 2.0 in one sample
}

TEST_CASE("a stationary source takes the static path without changing the result") {
    /*  process() has two inner loops: a ramp, and a flat multiply for the (very common)
     *  case of a source that has not moved. They must be indistinguishable from outside,
     *  and the flat one skips silent channels entirely -- so it also has to leave whatever
     *  was already in those buffers alone. */
    Encoder e;
    e.prepare(1);
    e.setTarget(fromAzEl(0.7, 0.3), 0.4);
    e.snap();  // target == current: every dg is exactly zero

    const int n = 32;
    std::vector<float> in(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = 0.25f * static_cast<float>(i % 7) - 0.5f;

    std::vector<std::vector<float>> out(4, std::vector<float>(static_cast<std::size_t>(n), 0.125f));
    auto p = ptrsOf(out);
    e.process(in.data(), p, n);

    //  Reconstruct the gains independently and demand an exact match, not an approximate
    //  one: a flat gain has no interpolation to hide a discrepancy behind.
    std::vector<double> y(4);
    std::vector<double> w(2);
    capWeights(0.4, 1, w);
    shSN3D(unit(fromAzEl(0.7, 0.3)), 1, y);
    for (std::size_t c = 0; c < 4; ++c) {
        const float g = static_cast<float>(y[c] * w[acnOrder(static_cast<int>(c))]);
        for (int i = 0; i < n; ++i)
            CHECK(out[c][static_cast<std::size_t>(i)] ==
                  doctest::Approx(0.125f + in[static_cast<std::size_t>(i)] * g).epsilon(1e-6));
    }
}

TEST_CASE("a silent channel is left untouched rather than zeroed") {
    Encoder e;
    e.prepare(1);
    e.setTarget(fromAzEl(0, 0), 0.0);  // dead front: channels 1 (Y) and 2 (Z) are zero
    e.snap();

    std::vector<float> in(8, 1.0f);
    std::vector<std::vector<float>> out(4, std::vector<float>(8, 0.75f));
    auto p = ptrsOf(out);
    e.process(in.data(), p, 8);

    CHECK(out[1][3] == doctest::Approx(0.75f));  // pre-existing content survives
    CHECK(out[2][3] == doctest::Approx(0.75f));
}

TEST_CASE("process adds into the output rather than overwriting it") {
    Encoder e;
    e.prepare(0);
    e.setTarget(fromAzEl(0, 0), 0.0);
    e.snap();

    std::vector<float> in(8, 1.0f);
    std::vector<std::vector<float>> out(1, std::vector<float>(8, 0.5f));
    auto p = ptrsOf(out);
    e.process(in.data(), p, 8);
    CHECK(out[0][4] == doctest::Approx(1.5f));
}

TEST_CASE("rendering is deterministic — the property golden-file regression rests on") {
    auto render = [] {
        Encoder e;
        e.prepare(3);
        std::vector<float> in(1024);
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = std::sin(static_cast<float>(i) * 0.03f);
        std::vector<std::vector<float>> out(16, std::vector<float>(1024, 0.0f));
        auto p = ptrsOf(out);
        double phase = 0.0;
        for (int b = 0; b < 8; ++b) {
            e.setTarget(fromAzEl(phase, phase * 0.3), 0.2);
            if (b == 0) e.snap();
            std::vector<float*> block;
            for (auto& c : out) block.push_back(c.data() + b * 128);
            e.process(in.data() + b * 128, block, 128);
            phase += 0.11;
        }
        return out;
    };
    const auto a = render(), b = render();
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i)
            REQUIRE(a[c][i] == b[c][i]);  // bit-identical, not approximately equal
}

TEST_CASE("gain is part of the target: it scales every channel, and it is ramped, not stepped") {
    Encoder unity, half;
    unity.prepare(3);
    half.prepare(3);
    const Vec3 at = fromAzEl(0.7, 0.3);
    unity.setTarget(at, 0.4);
    unity.snap();
    half.setTarget(at, 0.4, 0, 0.5);
    half.snap();
    for (std::size_t c = 0; c < unity.gains().size(); ++c) CHECK(half.gains()[c] == unity.gains()[c] * 0.5);

    /*  A drop to a tenth, asked for over 256 samples. Multiplied on after the encoder, once a control
        step, the output falls by 0.9 of full scale between one sample and the next; ramped, no two
        neighbouring samples are further apart than the whole change shared over the ramp. */
    Encoder e;
    e.prepare(0);
    e.setTarget(at, 0.0);
    e.snap();
    e.setTarget(at, 0.0, 256, 0.1);
    std::vector<float> in(256, 1.0f), w(256, 0.0f);
    std::vector<float*> out{w.data()};
    //  in uneven pieces, as a host would hand them over
    e.process(in.data(), out, 100);
    out[0] = w.data() + 100;
    e.process(in.data() + 100, out, 156);
    double worst = 0.0;
    for (std::size_t i = 1; i < w.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(w[i] - w[i - 1])));
    CHECK(worst < 1.5 * 0.9 / 256.0);
    CHECK(w.front() > 0.99f);
    CHECK(std::abs(w.back() - 0.1f) < 0.01f);
}

// ---- a stereo input: two points in one target ---------------------------------------------------

/*  The point of the difference: a side signal aimed at +left and -right has nothing in W, at any
 *  width, spread or order, so a mono fold-down of the output is the mid alone, (L+R)/2. A side
 *  encoded as a wide blob at the source would instead have W gain 1 whatever its width, folding to
 *  the left channel alone and dropping the right's content. Catches either weight's sign being
 *  lost, and the two points being encoded at different widths. */
TEST_CASE("a side aimed at the difference of two points is absent from W") {
    for (const int order : {1, 3, 5}) {
        for (const double spreadDeg : {10.0, 90.0, 180.0}) {
            Encoder side;
            side.prepare(order);
            const auto pair = stereoPairAbout({1.0, 0.0, 0.0}, spreadDeg * kDeg2Rad);
            side.setTargetPair(pair.left, 1.0, pair.right, -1.0, 40.0 * kDeg2Rad);
            side.snap();
            CHECK(std::abs(side.gains()[0]) < 1e-12);
            //  and it is not nothing: left and right differ in the first-order left-right channel
            CHECK(std::abs(side.gains()[1]) > 1e-3);
        }
    }
}

/*  Catches the pair target drifting from the single-point one: two equal halves of one direction
 *  are that direction, to the last bit of what the ramp will use. */
TEST_CASE("two halves of one point are that point") {
    Encoder one, pair;
    one.prepare(3);
    pair.prepare(3);
    const Vec3 d = unit(Vec3{0.3, -0.8, 0.5});
    one.setTarget(d, 25.0 * kDeg2Rad, 0, 0.7);
    pair.setTargetPair(d, 0.5, d, 0.5, 25.0 * kDeg2Rad, 0, 0.7);
    one.snap();
    pair.snap();
    for (std::size_t c = 0; c < one.gains().size(); ++c)
        CHECK(pair.gains()[c] == doctest::Approx(one.gains()[c]).epsilon(1e-12));
}

/*  An offset in azimuth closes as the source climbs: 90 degrees of it is 14 at elevation 80. The
 *  pair is turned about the source along the level great circle, so it keeps its width -- and over
 *  the last ten degrees to the pole, where "left" flips sign as the source passes over, it closes
 *  smoothly to a point rather than inverting the side. Catches the pair being an azimuth offset, the
 *  pole zone going, and left and right being swapped (y is left in this project's coordinates). */
TEST_CASE("left and right stay a spread apart until the last ten degrees, and never swap") {
    const auto between = [](const StereoPair& p) {
        return std::acos(std::clamp(dot(p.left, p.right), -1.0, 1.0)) / kDeg2Rad;
    };
    const double spread = 90.0 * kDeg2Rad;

    const auto front = stereoPairAbout({1.0, 0.0, 0.0}, spread);
    CHECK(front.left.y > 0.5);  // +y is left
    CHECK(front.right.y < -0.5);
    CHECK(between(front) == doctest::Approx(90.0));

    for (const double el : {0.0, 40.0, 60.0, 80.0})
        CHECK(between(stereoPairAbout(fromAzEl(0.7, el * kDeg2Rad), spread)) == doctest::Approx(90.0).epsilon(1e-6));

    const double at85 = between(stereoPairAbout(fromAzEl(0.7, 85.0 * kDeg2Rad), spread));
    const double at88 = between(stereoPairAbout(fromAzEl(0.7, 88.0 * kDeg2Rad), spread));
    CHECK(at85 < 90.0);
    CHECK(at88 < at85);
    CHECK(between(stereoPairAbout({0.0, 0.0, 1.0}, spread)) == doctest::Approx(0.0));

    //  every direction in the pair is a direction
    const auto high = stereoPairAbout(fromAzEl(-2.0, 87.0 * kDeg2Rad), 180.0 * kDeg2Rad);
    CHECK(length(high.left) == doctest::Approx(1.0));
    CHECK(length(high.right) == doctest::Approx(1.0));
}

/*  The level of two points sharing one signal: centred material goes to both; `1/sqrt(2(1+overlap))`
 *  of each keeps what they add up to at one point's energy, whatever lies between them. Catches the
 *  overlap being computed at the wrong width or order, from the angle alone, or without the (2n+1)
 *  that makes it energy -- a plain SN3D dot product reads up to 1.35 dB low at order 3. */
TEST_CASE("the overlap of two encodes is the cosine between their gain vectors, in energy") {
    for (const int order : {1, 3}) {
        for (const double widthDeg : {0.0, 60.0}) {
            const Vec3 a = fromAzEl(0.4, 0.2), b = fromAzEl(-0.9, 0.5);
            Encoder ea, eb;
            ea.prepare(order);
            eb.prepare(order);
            ea.setTarget(a, widthDeg * kDeg2Rad);
            eb.setTarget(b, widthDeg * kDeg2Rad);
            ea.snap();
            eb.snap();
            //  a sound field's energy weights order n by (2n + 1); channel c is of order floor(sqrt(c))
            double ab = 0.0, aa = 0.0;
            for (std::size_t c = 0; c < ea.gains().size(); ++c) {
                const double n = std::floor(std::sqrt(static_cast<double>(c)));
                ab += (2.0 * n + 1.0) * ea.gains()[c] * eb.gains()[c];
                aa += (2.0 * n + 1.0) * ea.gains()[c] * ea.gains()[c];
            }
            CHECK(encodeOverlap(a, b, widthDeg * kDeg2Rad, order) == doctest::Approx(ab / aa).epsilon(1e-9));
        }
    }
    CHECK(encodeOverlap({1, 0, 0}, {1, 0, 0}, 0.3, 3) == doctest::Approx(1.0));
}
