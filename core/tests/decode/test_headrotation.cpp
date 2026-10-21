// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "bambi/decode/headrotation.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

namespace {
/// A plane wave from `dir`, `frames` frames of the signal s(f), at `order`.
std::vector<float> planeWave(Vec3 dir, int order, int frames, unsigned seed) {
    const int C = numChannels(order);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(dir, order, y);
    const std::vector<float> s = noise(1, frames, seed);
    std::vector<float> x(static_cast<std::size_t>(frames * C));
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < C; ++c)
            x[static_cast<std::size_t>(f * C + c)] =
                s[static_cast<std::size_t>(f)] * static_cast<float>(y[static_cast<std::size_t>(c)]);
    return x;
}

std::vector<float> run(HeadRotation& r, const std::vector<float>& in, int channels, int frames,
                       const std::vector<int>& cuts) {
    std::vector<float> out(in.size());
    std::size_t cut = 0;
    for (int done = 0; done < frames;) {
        const int n = std::min(cuts[cut++ % cuts.size()], frames - done);
        r.process(in.data() + static_cast<std::size_t>(done * channels),
                  out.data() + static_cast<std::size_t>(done * channels), n);
        done += n;
    }
    return out;
}
}  // namespace

TEST_CASE("the field turns against the head: a source stays put in the room while the head turns") {
    //  Catches: the head's rotation applied to the field instead of its inverse, or a stale set of blocks.
    const int order = 3, C = 16, frames = 48000;
    const std::vector<float> in = planeWave(direction(30, 0), order, frames, 1);
    HeadRotation r;
    REQUIRE(r.prepare(order, 48000.0));
    r.setOrientation(rotationAbout({0, 0, 1}, 90.0 * kDeg2Rad));  // the head looks left
    const std::vector<float> out = run(r, in, C, frames, {128});
    // After a second the glide is over; the source, at 30 degrees in the room, is at -60 for the head.
    const std::vector<float> want = planeWave(direction(-60, 0), order, frames, 1);
    double worst = 0;
    for (std::size_t i = static_cast<std::size_t>(frames / 2) * C; i < out.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(out[i] - want[i])));
    CHECK(worst < 1e-5);
    const Mat3 now = r.current();
    CHECK(std::abs(now[0]) < 1e-9);
    CHECK(now[1] == doctest::Approx(-1.0));
}

TEST_CASE("an identity orientation passes the field through unchanged, bit for bit") {
    //  Catches: no blocks built before the first turn, so the field is silent until the head moves.
    const int order = 2, C = 9, frames = 1000;
    const std::vector<float> in = noise(C, frames, 4);
    HeadRotation r;
    REQUIRE(r.prepare(order, 48000.0));
    r.setOrientation(kIdentity3);
    const std::vector<float> out = run(r, in, C, frames, {64});
    CHECK(out == in);
}

TEST_CASE("a jump in orientation glides, never steps, and gives the same bits however the frames are cut") {
    //  Catches: a turn switched at once, a glide stepping per call instead of per hop along the stream.
    const int order = 3, C = 16, frames = 12000;
    const std::vector<float> in = planeWave(direction(0, 0), order, frames, 2);
    // A steady source: the omni channel is the signal, the others follow the direction. Use a constant signal to
    // watch the gains move.
    std::vector<float> steady(in.size());
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(direction(0, 0), order, y);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < C; ++c)
            steady[static_cast<std::size_t>(f * C + c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    HeadRotation a, b;
    REQUIRE(a.prepare(order, 48000.0));
    REQUIRE(b.prepare(order, 48000.0));
    //  played at rest first -- the first orientation is taken at once -- so the turn below is a jump in mid-stream
    //  for the glide to take
    a.setOrientation(kIdentity3);
    b.setOrientation(kIdentity3);
    std::vector<float> rest(static_cast<std::size_t>(256 * C));
    a.process(steady.data(), rest.data(), 256);
    b.process(steady.data(), rest.data(), 256);
    const Mat3 head = rotationAbout({0, 0, 1}, 90.0 * kDeg2Rad);
    a.setOrientation(head);
    b.setOrientation(head);
    const std::vector<float> outA = run(a, steady, C, frames, {128}),
                             outB = run(b, steady, C, frames, {1, 7, 64, 100, 513});
    CHECK(outA == outB);
    // The y channel (ACN 1) goes from 0 to -1 (the source now on the right): monotone-ish, no step wider than a hop's share.
    float biggest = 0;
    for (int f = 1; f < frames; ++f)
        biggest = std::max(biggest, std::abs(outA[static_cast<std::size_t>(f * C + 1)] -
                                             outA[static_cast<std::size_t>((f - 1) * C + 1)]));
    CHECK(biggest < 0.005f);  // a glide: four degrees in its steepest hop, where a jump is the whole turn in one
    CHECK(outA[static_cast<std::size_t>((frames - 1) * C + 1)] == doctest::Approx(-1.0).epsilon(1e-4));
}
