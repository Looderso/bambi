// SPDX-License-Identifier: GPL-3.0-or-later
//
//  A synced LFO is a function of the song position at its step. These check that the position a
//  step is given depends on the step's sample and on nothing about how the host cut its blocks.

#include <cmath>
#include <cstdint>
#include <vector>

#include "bambi/mod/beatclock.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kRate = 48000.0;
constexpr int kHop = 256;

/// The host's reading at a sample, computed the way a host does: seconds, then beats.
double hostPpq(std::int64_t sample, double bpm) { return static_cast<double>(sample) / kRate * bpm / 60.0; }

/*  Play `total` samples from `start` in blocks of `block`, each block optionally split into
 *  sub-blocks that all carry the HOST block's reading, as a CLAP wrapper does. Returns the position
 *  of every control step, pinned to the timeline as the control grid pins it. */
std::vector<double> play(std::int64_t start, std::int64_t total, std::int64_t block, double bpm,
                         std::vector<std::int64_t> split = {}) {
    BeatClock clock;
    std::vector<double> steps;
    for (std::int64_t t = start; t < start + total; t += block) {
        const auto n = std::min(block, start + total - t);
        const double ppq = hostPpq(t, bpm);  // one reading per host block
        std::vector<std::int64_t> parts = split.empty() ? std::vector<std::int64_t>{n} : split;
        std::int64_t at = t;
        for (auto m : parts) {
            m = std::min(m, t + n - at);
            if (m <= 0) break;
            clock.observe(at, ppq, bpm, kRate);
            for (std::int64_t s = at; s < at + m; ++s)
                if (s == start || s % kHop == 0) steps.push_back(clock.ppqAt(s));
            at += m;
        }
    }
    return steps;
}

/// The same walk, positioned by extrapolating from the block's ppq plus the distance travelled.
std::vector<double> playExtrapolated(std::int64_t start, std::int64_t total, std::int64_t block, double bpm) {
    std::vector<double> steps;
    for (std::int64_t t = start; t < start + total; t += block) {
        const auto n = std::min(block, start + total - t);
        const double ppq = hostPpq(t, bpm);
        for (std::int64_t s = t; s < t + n; ++s)
            if (s == start || s % kHop == 0) steps.push_back(ppq + (static_cast<double>(s - t) / kRate) * bpm / 60.0);
    }
    return steps;
}

}  // namespace

TEST_CASE("a step's song position does not depend on the host's block size") {
    //  A non-zero start and a tempo whose samples are not a round number of beats: rounding shows.
    const std::int64_t start = 123457, total = 10 * 48000;
    const double bpm = 128.0;
    const auto base = play(start, total, 1, bpm);
    REQUIRE(base.size() > 1000);
    for (std::int64_t block : {64, 1000, 480, 4096}) {
        CAPTURE(block);
        CHECK(play(start, total, block, bpm) == base);
    }
    //  And it is the song's position, not merely a consistent one.
    CHECK(base.front() == hostPpq(start, bpm));
    CHECK(std::abs(base.back() - hostPpq(start + total - 1 - (start + total - 1) % kHop, bpm)) < 1e-9);
}

TEST_CASE("the old extrapolation from the block's start is caught by the same comparison") {
    //  The mutation this file exists for: `ppqHere` from the block-start ppq. Shown here to differ,
    //  so the check above is known to be able to fail.
    const std::int64_t start = 123457, total = 10 * 48000;
    const double bpm = 128.0;
    const auto base = playExtrapolated(start, total, 1, bpm);
    bool anyDiffers = false;
    for (std::int64_t block : {64, 1000}) anyDiffers = anyDiffers || playExtrapolated(start, total, block, bpm) != base;
    CHECK(anyDiffers);
}

TEST_CASE("sub-blocks carrying their host block's stale reading change nothing") {
    //  Catches: taking a reading that has not moved as a new one -- the second sub-block
    //  would re-anchor its own sample to the host block's first sample's position.
    const std::int64_t start = 5000, total = 4 * 48000;
    const double bpm = 97.0;
    CHECK(play(start, total, 1000, bpm, {100, 350, 550}) == play(start, total, 1000, bpm));
}

TEST_CASE("a new tempo is taken at the block that reports it") {
    BeatClock clock;
    clock.observe(0, 0.0, 120.0, kRate);
    CHECK(clock.ppqAt(48000) == doctest::Approx(2.0));
    //  At 1 s the host says 140 bpm, from 2 quarter notes.
    clock.observe(48000, 2.0, 140.0, kRate);
    CHECK(clock.ppqAt(48000 + 48000) == doctest::Approx(2.0 + 140.0 / 60.0));
}

TEST_CASE("a position that disagrees by more than half a sample moves the anchor; less does not") {
    //  Catches: a tolerance of zero (every block re-anchors, and the block size shows again)
    //  or none at all (a host playing faster than its samples, varispeed, is never followed).
    const double bpm = 120.0, perSample = bpm / (60.0 * kRate);
    BeatClock clock;
    clock.observe(0, 0.0, bpm, kRate);
    const double predicted = clock.ppqAt(1000);
    clock.observe(1000, predicted + 0.3 * perSample, bpm, kRate);
    CHECK(clock.ppqAt(1000) == predicted);  // kept: the anchor did not move
    clock.observe(2000, clock.ppqAt(2000) + 3.0 * perSample, bpm, kRate);
    CHECK(clock.ppqAt(2000) == doctest::Approx(2000 * perSample + 3.0 * perSample));
}

TEST_CASE("forgetting takes the next reading whole") {
    BeatClock clock;
    clock.observe(0, 0.0, 120.0, kRate);
    clock.forget();
    clock.observe(0, 7.25, 120.0, kRate);  // a locate: the same sample, another place in the song
    CHECK(clock.ppqAt(0) == 7.25);
}

TEST_CASE("stopped, the position holds while the samples count on") {
    //  The grid hands a stopped transport's reading at tempo 0. Catches: a stopped reading
    //  skipped as "not moved" and the old tempo kept -- the position then runs on at it.
    BeatClock clock;
    clock.observe(0, 0.0, 120.0, kRate);
    clock.observe(48000, 2.0, 0.0, kRate);  // stopped at 2 quarter notes
    clock.observe(96000, 2.0, 0.0, kRate);
    CHECK(clock.ppqAt(96000) == 2.0);
    CHECK(clock.ppqAt(200000) == 2.0);
}

TEST_CASE("a tempo that differs in its last bit is the same tempo") {
    //  Catches: an exact comparison -- a tempo map's rounding would re-anchor every block,
    //  and the block size would show again.
    const double bpm = 128.0, jittered = std::nextafter(bpm, 200.0);
    BeatClock clock;
    clock.observe(0, 0.0, bpm, kRate);
    const double kept = clock.ppqAt(1000);
    clock.observe(1000, kept + 1e-12, jittered, kRate);
    CHECK(clock.ppqAt(1000) == kept);
}
