// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "bambi/echo/control.hpp"
#include "bambi/echo/engine.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kFs = 48000.0;

/*  The band as open as it goes. It is never quite absent -- the high cut's one-pole is capped at 0.45
 *  of the sample rate, so an impulse comes out as a few samples, not one -- but with no low cut it
 *  passes DC at exactly 1, so a repeat's samples still ADD UP to its closed-form level. */
TapSettings openPass() {
    TapSettings s;
    s.lowCutHz = 0.0;
    s.highCutHz = 1.0e9;
    return s;
}

EchoTapFrame tap(int period, int offset, double level, double feedback, double spinRad = 0.0) {
    EchoTapFrame t;
    t.on = true;
    t.periodSamples = period;
    t.offsetSamples = offset;
    t.level = level;
    t.feedback = feedback;
    t.pass = openPass();
    t.pass.spinRad = spinRad;
    return t;
}

/*  Runs an engine the way a host would: a frame every 256 samples on an absolute grid, the audio
 *  handed over in whatever pieces the test asks for. */
struct Host {
    EchoEngine engine;
    EchoFrame frame;
    std::function<void(int, EchoFrame&)> evolve;  ///< called with the hop's number before each frame is handed over
    int order;
    std::vector<float> out;
    explicit Host(int n) : order(n) { engine.prepare(n, kFs); }

    void run(const std::vector<float>& in, const std::vector<int>& pieces) {
        const int C = engine.channels(), frames = static_cast<int>(in.size()) / C;
        out.assign(in.size(), 0.0f);
        int pos = 0;
        std::size_t piece = 0;
        while (pos < frames) {
            int n = std::min(frames - pos, pieces[piece++ % pieces.size()]);
            while (n > 0) {
                if (pos % kEchoHop == 0) {
                    if (evolve) evolve(pos / kEchoHop, frame);
                    engine.setFrame(frame);
                }
                const int m = std::min(n, kEchoHop - pos % kEchoHop);
                engine.process(in.data() + static_cast<std::size_t>(pos) * C,
                               out.data() + static_cast<std::size_t>(pos) * C, m);
                pos += m;
                n -= m;
            }
        }
    }
};

std::vector<float> impulseAt(int order, int frames, Vec3 direction, int at = 0) {
    const int C = numChannels(order);
    std::vector<float> in(static_cast<std::size_t>(frames * C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(direction, order, y);
    for (int c = 0; c < C; ++c)
        in[static_cast<std::size_t>(at * C + c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    return in;
}

std::vector<float> noise(int order, int frames, unsigned seed = 3u) {
    std::vector<float> f(static_cast<std::size_t>(numChannels(order) * frames));
    unsigned s = seed;
    for (float& v : f) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(s >> 8) % 20001 - 10000) / 20000.0f;
    }
    return f;
}

}  // namespace

TEST_CASE("repeat j lands at offset + j periods, at level times feedback to the j-1, turned j times") {
    const int order = 3, C = 16, period = 1000, offset = 300;
    Host host(order);
    host.frame.taps[0] = tap(period, offset, 0.5, 0.8, 30.0 * kDeg2Rad);
    const Vec3 source = fromAzEl(20.0 * kDeg2Rad, 0.0);
    host.run(impulseAt(order, 6000, source), {128});

    //  nothing is heard untransformed, or early
    for (int f = 0; f < offset + period; ++f)
        for (int c = 0; c < C; ++c) REQUIRE(host.out[static_cast<std::size_t>(f * C + c)] == 0.0f);

    std::vector<double> want(C);
    for (int j = 1; j <= 4; ++j) {
        const int at = offset + j * period;
        shSN3D(fromAzEl((20.0 + 30.0 * j) * kDeg2Rad, 0.0), order, want);
        const double gain = 0.5 * std::pow(0.8, j - 1);
        for (int c = 0; c < C; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 60; ++k) sum += host.out[static_cast<std::size_t>((at + k) * C + c)];
            CHECK(std::abs(sum - gain * want[static_cast<std::size_t>(c)]) < 1e-5);
            //  and it starts there, not a sample sooner
            CHECK(host.out[static_cast<std::size_t>((at - 1) * C + c)] == doctest::Approx(0.0).epsilon(1e-9));
        }
        CHECK(std::abs(host.out[static_cast<std::size_t>(at * C)]) > 0.1 * gain);
    }
}

TEST_CASE("the first frame after a reset is taken at once, not faded in") {
    //  A repeat inside the first control hop is already at its level: a fade there would attenuate the
    //  first repeat of every short tap, at every play, locate and loop.
    const int order = 0;
    Host host(order);
    host.frame.taps[0] = tap(100, 0, 0.5, 0.0);
    std::vector<float> in(2000, 0.0f);
    in[0] = 1.0f;
    host.run(in, {128});
    double sum = 0.0;
    for (int k = 100; k < 160; ++k) sum += host.out[static_cast<std::size_t>(k)];
    CHECK(sum == doctest::Approx(0.5).epsilon(1e-5));
}

TEST_CASE("the taps add up, do not feed each other, and level leaves the tail alone") {
    const int order = 1, frames = 8000;
    const auto in = noise(order, frames);
    Host a(order), b(order), both(order);
    a.frame.taps[0] = tap(700, 0, 0.7, 0.6);
    b.frame.taps[2] = tap(1100, 250, 0.4, 0.5, 1.0);
    both.frame.taps[0] = a.frame.taps[0];
    both.frame.taps[2] = b.frame.taps[2];
    a.run(in, {256});
    b.run(in, {256});
    both.run(in, {256});
    for (std::size_t k = 0; k < in.size(); ++k) CHECK(std::abs(both.out[k] - (a.out[k] + b.out[k])) < 1e-5);

    //  level is outside the loop: halve it and everything heard halves, the tail in flight included
    Host quiet(order);
    quiet.frame.taps[0] = tap(700, 0, 0.35, 0.6);
    quiet.run(in, {256});
    for (std::size_t k = 0; k < in.size(); ++k) CHECK(std::abs(quiet.out[k] - 0.5f * a.out[k]) < 1e-5);
}

TEST_CASE("a render does not depend on how it was cut up, down to a period shorter than any block") {
    /*  An offline bounce must match playback, with feedback. Periods of 1, 7 and 257 samples are
        shorter than a chunk, a hop and most blocks; the pieces include 1, a prime, the hop's
        neighbours and 1024. Catches: a chunk that reads what it has just written, a ramp advanced
        per call, and a frame taken at a block's start rather than on the grid. */
    const int order = 3, frames = 5000;
    const auto in = noise(order, frames);
    const auto render = [&](const std::vector<int>& pieces) {
        Host host(order);
        host.frame.taps[0] = tap(1, 0, 0.5, 0.5);
        host.frame.taps[1] = tap(7, 3, 0.6, 0.7, 0.4);
        host.frame.taps[2] = tap(257, 100, 0.8, 0.9, -0.2);
        host.frame.taps[2].pass.skew = 0.4;
        host.frame.taps[2].pass.axis = unit(Vec3{0.3, 0.5, 0.6});
        host.frame.taps[2].pass.axisIsPole = false;
        host.frame.taps[2].pass.lowCutHz = 100.0;
        host.frame.taps[2].pass.highCutHz = 5000.0;
        host.frame.taps[3] = tap(2000, 0, 0.3, 0.95);
        //  and everything a frame can move, moving: levels and feedbacks brought in over each hop, a
        //  period that changes, a tap switched off and on again, a spin that turns
        host.evolve = [](int hop, EchoFrame& f) {
            f.taps[1].level = 0.6 + 0.3 * std::sin(0.7 * hop);
            f.taps[2].feedback = 0.6 + 0.3 * std::cos(0.3 * hop);
            f.taps[2].pass.spinRad = 0.05 * hop;
            f.taps[3].periodSamples = hop < 8 ? 2000 : 1500 + 10 * hop;
            f.taps[0].on = hop < 5 || hop > 9;
            f.sendAmount = 0.5 + 0.4 * std::sin(0.9 * hop);
        };
        host.run(in, pieces);
        return host.out;
    };
    const auto base = render({64});
    for (const std::vector<int>& pieces : {std::vector<int>{1}, {2, 7, 255, 256, 257, 333}, {1000, 1024, 13}})
        CHECK(std::memcmp(base.data(), render(pieces).data(), base.size() * sizeof(float)) == 0);
    double heard = 0.0;
    for (const float v : base) heard += std::abs(v);
    CHECK(heard > 100.0);
}

TEST_CASE("after a reset the engine is the one that was never played") {
    //  A bounce from here equals playback from here, whatever rang before. Catches: a band state, the
    //  input line or a ring surviving the reset, and a level faded in where it should be taken at once.
    const int order = 3, frames = 4000;
    const auto before = noise(order, 6000, 11u), after = noise(order, frames, 5u);
    const auto setUp = [](Host& h) {
        h.frame.taps[0] = tap(900, 400, 0.7, 0.9, 0.3);
        h.frame.taps[0].pass.lowCutHz = 150.0;
        h.frame.taps[0].pass.highCutHz = 4000.0;
    };
    Host played(order), fresh(order);
    setUp(played);
    setUp(fresh);
    played.run(before, {128});
    played.engine.reset();
    played.run(after, {128});
    fresh.run(after, {128});
    CHECK(std::memcmp(played.out.data(), fresh.out.data(), fresh.out.size() * sizeof(float)) == 0);
}

TEST_CASE("what is not a number does not go round, and a loop that grows is held and counted") {
    const int order = 1, C = 4, frames = 3000;
    auto poisoned = noise(order, frames), cleaned = poisoned;
    for (const int at : {100, 101}) {
        poisoned[static_cast<std::size_t>(at * C + 1)] = std::numeric_limits<float>::quiet_NaN();
        poisoned[static_cast<std::size_t>(at * C + 2)] = std::numeric_limits<float>::infinity();
        cleaned[static_cast<std::size_t>(at * C + 1)] = cleaned[static_cast<std::size_t>(at * C + 2)] = 0.0f;
    }
    Host a(order), b(order);
    a.frame.taps[0] = b.frame.taps[0] = tap(500, 0, 1.0, 0.95);
    a.run(poisoned, {128});
    b.run(cleaned, {128});
    for (const float v : a.out) REQUIRE(std::isfinite(v));
    CHECK(std::memcmp(a.out.data(), b.out.data(), a.out.size() * sizeof(float)) == 0);

    //  feedback the engine would never be given, to make a loop grow: held at the limit, and counted
    Host loud(order);
    loud.frame.taps[0] = tap(20, 0, 1.0, 1.0);
    loud.frame.taps[0].pass.skew = 0.0;
    std::vector<float> drive(static_cast<std::size_t>(C * 48000),
                             10.0f);  // +1 a pass at unity feedback: past the limit in a second
    loud.run(drive, {512});
    for (const float v : loud.out) REQUIRE(std::isfinite(v));
    CHECK(loud.engine.clamped() > 0);
    CHECK(a.engine.clamped() == 0);
}

TEST_CASE("a tap switched off fades out and sleeps, and wakes with nothing in it") {
    const int order = 1, C = 4;
    Host host(order);
    host.frame.taps[0] = tap(600, 0, 1.0, 0.9);
    host.run(noise(order, 3000), {128});
    CHECK_FALSE(host.engine.asleep(0));

    host.frame.taps[0].on = false;
    const std::vector<float> silence(static_cast<std::size_t>(C * 1024), 0.0f);
    host.run(silence, {128});
    //  it faded over the hop rather than stopping dead, and then slept
    CHECK(std::abs(host.out[static_cast<std::size_t>(10 * C)]) > 0.0f);
    CHECK(host.engine.asleep(0));
    for (std::size_t k = static_cast<std::size_t>(2 * kEchoHop * C); k < host.out.size(); ++k)
        REQUIRE(host.out[k] == 0.0f);

    //  woken with silence going in, nothing comes out: the tail it slept on is gone
    host.frame.taps[0].on = true;
    host.run(silence, {128});
    for (const float v : host.out) REQUIRE(v == 0.0f);
}

TEST_CASE("a period that changes while the tap plays keeps its tail and does not click") {
    /*  A steady tone goes round a loop; the period changes; the output is crossfaded from the old
        read to the new over one hop. Measured as the largest step between neighbouring
        samples: no larger across the change than the signal's own, where a read that jumped at once
        -- or a ring cleared -- shows as a step the size of the signal itself. */
    const int order = 0, frames = 12000;
    std::vector<float> tone(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        tone[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 110.0 * i / kFs));
    Host host(order);
    host.frame.taps[0] = tap(2400, 0, 1.0, 0.0);
    host.engine.setFrame(host.frame);
    std::vector<float> out(tone.size());
    const int change = 20 * kEchoHop;
    for (int pos = 0; pos < frames; pos += kEchoHop) {
        if (pos == change) host.frame.taps[0].periodSamples = 1700;
        host.engine.setFrame(host.frame);
        host.engine.process(tone.data() + pos, out.data() + pos, std::min(kEchoHop, frames - pos));
    }
    double own = 0.0, across = 0.0;
    for (int i = 2401; i < change; ++i) own = std::max(own, static_cast<double>(std::abs(out[i] - out[i - 1])));
    for (int i = change; i < change + 2 * kEchoHop; ++i)
        across = std::max(across, static_cast<double>(std::abs(out[i] - out[i - 1])));
    CHECK(own > 0.001);
    CHECK(across < 2.0 * own);
    //  and the tail is still there after it
    double after = 0.0;
    for (int i = change + kEchoHop; i < change + 3 * kEchoHop; ++i)
        after = std::max(after, static_cast<double>(std::abs(out[i])));
    CHECK(after > 0.4);
}

TEST_CASE("the send decides what is echoed at all, once for every tap") {
    /*  A half facing front. A source in front of the listener echoes and one behind does not -- nearly:
        at order 3 the region is band-limited and rings, so "does not" is 15 dB down, measured, rather
        than silent. That is what a third-order field can say about a half, not a fault in the send.
        From its outside the two swap. Catches: the send never applied, applied after the line
        instead of before it, and its side ignored. */
    const int order = 3, C = 16;
    const auto energyOf = [&](Vec3 source, RegionSide side, double amount) {
        Host host(order);
        host.frame.taps[0] = tap(500, 0, 1.0, 0.0);
        host.frame.taps[2] = tap(800, 100, 1.0, 0.0);
        host.frame.send.kind = RegionKind::Spot;
        host.frame.send.size = kPi / 2;
        host.frame.send.softness = 20.0 * kDeg2Rad;
        host.frame.send.side = side;
        host.frame.sendAmount = amount;
        host.run(impulseAt(order, 3000, source), {128});
        double e = 0.0;
        for (const float v : host.out) e += static_cast<double>(v) * v;
        return e;
    };
    const Vec3 front = fromAzEl(0.0, 0.0), back = fromAzEl(kPi, 0.0);
    const double inFront = energyOf(front, RegionSide::Inside, 1.0), inBack = energyOf(back, RegionSide::Inside, 1.0);
    CHECK(inFront > 0.1);
    CHECK(inBack < 0.05 * inFront);
    CHECK(energyOf(back, RegionSide::Outside, 1.0) > 0.1);
    CHECK(energyOf(front, RegionSide::Outside, 1.0) < 0.05 * inFront);
    //  the slot's amount is a gain on what is sent
    CHECK(energyOf(front, RegionSide::Inside, 0.5) == doctest::Approx(0.25 * inFront).epsilon(1e-4));

    //  and a send whose only change is its SIDE is a changed send: a source behind, sent from the
    //  inside and barely heard, is heard once the slot is flipped while it plays
    Host host(order);
    host.frame.taps[0] = tap(300, 0, 1.0, 0.0);
    host.frame.send.kind = RegionKind::Spot;
    host.frame.send.size = kPi / 2;
    host.evolve = [](int hop, EchoFrame& f) { f.send.side = hop < 4 ? RegionSide::Inside : RegionSide::Outside; };
    auto in = impulseAt(order, 3000, back, 0);
    const auto later = impulseAt(order, 3000, back, 1500);
    for (std::size_t k = 0; k < in.size(); ++k) in[k] += later[k];
    host.run(in, {128});
    double early = 0.0, late = 0.0;
    for (int f = 0; f < 3000; ++f)
        for (int c = 0; c < C; ++c) {
            const double v = host.out[static_cast<std::size_t>(f * C + c)];
            (f < 1500 ? early : late) += v * v;
        }
    CHECK(late > 20.0 * early);
}

TEST_CASE("a frame handed over only when something changed renders as one handed over every hop would cut") {
    /*  A caller may skip setFrame when nothing moved -- at order 7 a frame costs 1 % of a core. Then
        the hop a change is ramped over can begin anywhere in the caller's pieces, and a chunk that
        straddled its end would take one decision for both sides of it. Pieces of 100, 256 and 1
        must agree. */
    const int order = 1;
    const auto in = noise(order, 4000);
    const auto render = [&](int piece) {
        EchoEngine engine;
        engine.prepare(order, kFs);
        EchoFrame frame;
        frame.taps[0] = tap(900, 0, 0.8, 0.7, 0.3);
        engine.setFrame(frame);
        std::vector<float> out(in.size());
        const int C = engine.channels();
        for (int pos = 0; pos < 4000;) {
            if (pos == 2048) {  // the one change, on the grid
                frame.taps[0].periodSamples = 700;
                frame.taps[0].level = 0.3;
                frame.sendAmount = 0.5;
                engine.setFrame(frame);
            }
            const int toChange = pos < 2048 ? 2048 - pos : 4000 - pos;
            const int n = std::min(piece, toChange);
            engine.process(in.data() + static_cast<std::size_t>(pos) * C,
                           out.data() + static_cast<std::size_t>(pos) * C, n);
            pos += n;
        }
        return out;
    };
    const auto base = render(256);
    CHECK(std::memcmp(base.data(), render(100).data(), base.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(base.data(), render(1).data(), base.size() * sizeof(float)) == 0);
}

TEST_CASE("a frame that comes early carries on from where the last one had got to") {
    //  Level asked down to nothing, and 100 samples into that hop asked back up: it turns round where
    //  it is, about 0.61, rather than jumping to either end. Steady input, no feedback, no low cut, so
    //  the output is the level. Catches: the early-frame path made dead.
    EchoEngine engine;
    engine.prepare(0, kFs);
    EchoFrame frame;
    frame.taps[0] = tap(40, 0, 1.0, 0.0);
    engine.setFrame(frame);
    const std::vector<float> ones(2000, 1.0f);
    std::vector<float> out(2000);
    engine.process(ones.data(), out.data(), 1024);
    frame.taps[0].level = 0.0;
    engine.setFrame(frame);
    engine.process(ones.data() + 1024, out.data() + 1024, 100);
    frame.taps[0].level = 1.0;
    engine.setFrame(frame);
    engine.process(ones.data() + 1124, out.data() + 1124, 400);
    CHECK(out[1123] == doctest::Approx(1.0 - 100.0 / 256.0).epsilon(0.02));
    CHECK(std::abs(out[1124] - out[1123]) < 0.02);  // no jump at the early frame
    CHECK(out[1124 + 300] == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("settings that are not numbers do nothing, and leave nothing behind") {
    //  Every pass setting, the level and the feedback are NaN for one hop mid-render. Unguarded, a NaN
    //  would stay in the band's state for as long as the tap lives.
    const int order = 3;
    const auto in = noise(order, 6000);
    Host host(order);
    host.frame.taps[0] = tap(600, 0, 0.8, 0.8, 0.4);
    host.frame.taps[0].pass.lowCutHz = 100.0;
    host.frame.taps[0].pass.highCutHz = 6000.0;
    const EchoTapFrame good = host.frame.taps[0];
    host.evolve = [good](int hop, EchoFrame& f) {
        f.taps[0] = good;
        if (hop == 6) {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            EchoTapFrame& t = f.taps[0];
            t.level = t.feedback = nan;
            t.pass.spinRad = t.pass.skew = t.pass.blurRad = t.pass.lowCutHz = t.pass.highCutHz = nan;
            t.pass.axis = {nan, 0.0, 1.0};
            t.pass.axisIsPole = false;
        }
    };
    host.run(in, {128});
    for (const float v : host.out) REQUIRE(std::isfinite(v));
    double late = 0.0;
    for (std::size_t k = host.out.size() * 3 / 4; k < host.out.size(); ++k) late += std::abs(host.out[k]);
    CHECK(late > 1.0);  // and the tap is still playing afterwards
}

TEST_CASE("a spin that is not a number is no spin, not silence") {
    //  Unguarded, the band would turn a NaN spin into silence. The tap should play on, unturned.
    const int order = 1, C = 4;
    Host host(order);
    host.frame.taps[0] = tap(300, 0, 1.0, 0.0, 0.5);
    host.evolve = [](int hop, EchoFrame& f) {
        f.taps[0].pass.spinRad = hop >= 4 ? std::numeric_limits<double>::quiet_NaN() : 0.5;
    };
    host.run(noise(order, 4000), {128});
    //  in the channels a spin touches -- Y and X, m = -1 and +1; the omni channel never noticed
    double during = 0.0;
    for (int f = 8 * kEchoHop; f < 14 * kEchoHop; ++f)
        for (const int c : {1, 3}) during += std::abs(host.out[static_cast<std::size_t>(f * C + c)]);
    CHECK(during > 10.0);
}

TEST_CASE("the send's amount is brought in over the hop, not switched") {
    //  Steady input, a short tap with no feedback and no low cut: the output is what was sent a period
    //  ago. Dropped from 1 to 0.2, it falls over 256 samples -- 0.003 a sample -- where a switch is one
    //  step of 0.8.
    EchoEngine engine;
    engine.prepare(0, kFs);
    EchoFrame frame;
    frame.taps[0] = tap(40, 0, 1.0, 0.0);
    engine.setFrame(frame);
    const std::vector<float> ones(3000, 1.0f);
    std::vector<float> out(3000);
    engine.process(ones.data(), out.data(), 1024);
    frame.sendAmount = 0.2;
    for (int pos = 1024; pos < 2048; pos += kEchoHop) {
        engine.setFrame(frame);
        engine.process(ones.data() + pos, out.data() + pos, kEchoHop);
    }
    double step = 0.0;
    for (int i = 1025; i < 2048; ++i) step = std::max(step, static_cast<double>(std::abs(out[i] - out[i - 1])));
    CHECK(out[1000] == doctest::Approx(1.0).epsilon(0.01));
    CHECK(out[2000] == doctest::Approx(0.2).epsilon(0.01));
    CHECK(step < 0.02);
}

TEST_CASE("Echo's output stage: the taps are trimmed and the input put back, and neither feeds back") {
    /*  The wet trims what the four taps add up to and the dry adds the input beside it, both after
        the rings are written -- so what goes round is unaffected, and turning the dry up does not
        echo itself.

        Catches: the dry written into the line (each repeat would carry a copy of the input, and the
        tail would grow), the wet applied before the feedback write (it would become a second
        feedback control), and -60 dB treated as a gain rather than silence. */
    EchoEngine engine;
    engine.prepare(3, 48000.0);
    const int C = numChannels(3), frames = 24000;
    std::vector<float> in(static_cast<std::size_t>(frames) * C, 0.0f), out(in.size());
    in[0] = 1.0f;  // one impulse on the omni channel

    EchoSettings s = EchoSettings::defaults();
    s.taps[0].feedbackDb = -6.0;

    const auto render = [&](double dryDb, double wetDb) {
        EchoSettings settings = s;
        settings.dryDb = dryDb;
        settings.wetDb = wetDb;
        EchoResolver resolver;
        engine.reset();
        resolver.reset();
        for (int at = 0; at < frames; at += kEchoHop) {
            engine.setFrame(resolver.resolve(settings, 120.0, 48000.0, 3));
            engine.process(in.data() + static_cast<std::size_t>(at) * C, out.data() + static_cast<std::size_t>(at) * C,
                           std::min(kEchoHop, frames - at));
        }
        double dry = 0.0, taps = 0.0;
        for (int f = 0; f < frames; ++f) {
            const double v = out[static_cast<std::size_t>(f) * C];
            (f < 8 ? dry : taps) += v * v;
        }
        return std::pair<double, double>{dry, taps};
    };

    const auto [noDry, tapsAlone] = render(-60.0, 0.0);
    CHECK(noDry < 1.0e-9);  // nothing at the input's own moment: the engine is all wet
    CHECK(tapsAlone > 0.0);

    //  the dry appears and the repeats do not move: it is an output stage, not a path
    const auto [withDry, tapsAgain] = render(0.0, 0.0);
    CHECK(withDry == doctest::Approx(1.0).epsilon(0.02));
    CHECK(tapsAgain == doctest::Approx(tapsAlone).epsilon(1e-9));

    //  and the wet trims the taps without touching the dry
    const auto [dryStill, tapsHalf] = render(0.0, -6.0);
    CHECK(dryStill == doctest::Approx(withDry).epsilon(1e-9));
    CHECK(tapsHalf == doctest::Approx(tapsAlone * 0.25).epsilon(0.02));
}
