// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "bambi/dsp/biquad.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/reverb/engine.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kFs = 48000.0;

ReverbFrame preset(double size, RoomShape shape, double decay, double tone, double rough, double distance) {
    ReverbFrame s;
    s.room = {size, shape, decay, tone, rough};
    s.distance = distance;
    s.lowCutHz = 20.0;
    s.highCutHz = 20000.0;
    return s;
}
ReverbFrame hall() { return preset(20.0, RoomShape::Hall, 1.9, 0.5, 0.6, 6.0); }

//  The omni channel of the room's answer to an impulse from a direction.
std::vector<float> omniResponse(ReverbEngine& engine, Vec3 from, double seconds) {
    const int C = engine.channels(), frames = static_cast<int>(seconds * kFs);
    std::vector<float> in(static_cast<std::size_t>(frames * C), 0.0f), out(in.size());
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(from, 3, y);
    for (int c = 0; c < C; ++c) in[static_cast<std::size_t>(c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    engine.reset();
    for (int at = 0; at < frames; at += kReverbHop)
        engine.process(in.data() + at * C, out.data() + at * C, std::min(kReverbHop, frames - at));
    std::vector<float> w(static_cast<std::size_t>(frames));
    for (int f = 0; f < frames; ++f) w[static_cast<std::size_t>(f)] = out[static_cast<std::size_t>(f * C)];
    return w;
}

double energy(const std::vector<float>& w, double fromSeconds, double toSeconds) {
    double e = 0.0;
    for (int f = static_cast<int>(fromSeconds * kFs);
         f < std::min(static_cast<int>(toSeconds * kFs), static_cast<int>(w.size())); ++f)
        e += static_cast<double>(w[static_cast<std::size_t>(f)]) * w[static_cast<std::size_t>(f)];
    return e;
}

//  The mids alone -- 500 Hz to 2 kHz -- which is where a decay is set and measured.
std::vector<float> mids(const std::vector<float>& w) {
    const BiquadCoeffs hp = highPass(500.0, std::sqrt(0.5), kFs), lp = lowPass(2000.0, std::sqrt(0.5), kFs);
    BiquadState a, b, c, d;
    std::vector<float> out(w.size());
    for (std::size_t i = 0; i < w.size(); ++i)
        out[i] = static_cast<float>(run(lp, d, run(lp, c, run(hp, b, run(hp, a, w[i])))));
    return out;
}

double rt60(const std::vector<float>& w) {
    std::vector<double> edc(w.size());
    double sum = 0.0;
    for (std::size_t f = w.size(); f-- > 0;) {
        sum += static_cast<double>(w[f]) * w[f];
        edc[f] = sum;
    }
    int from = -1, to = -1;
    for (std::size_t f = 0; f < w.size(); ++f) {
        const double db = 10.0 * std::log10(edc[f] / edc[0] + 1e-300);
        if (from < 0 && db <= -5.0) from = static_cast<int>(f);
        if (to < 0 && db <= -35.0) {
            to = static_cast<int>(f);
            break;
        }
    }
    return from < 0 || to <= from ? 0.0 : 2.0 * (to - from) / kFs;
}

}  // namespace

const std::vector<Vec3> kAround{fromAzEl(0.3, 0.1), fromAzEl(2.0, -0.4), fromAzEl(-1.2, 0.6), fromAzEl(3.0, 0.0)};

TEST_CASE("the decay that is set is the decay of the whole room, at mid") {
    //  Measured: room 0.50 -> 0.53, hall 1.90 -> 1.91, cathedral 6.50 -> 6.18.
    ReverbEngine engine;
    engine.prepare(3, kFs);
    for (const ReverbFrame& s : {preset(5.0, RoomShape::Room, 0.5, 0.42, 0.75, 2.0), hall(),
                                 preset(38.0, RoomShape::Tall, 6.5, 0.35, 0.5, 14.0)}) {
        engine.set(s);
        const double measured = rt60(mids(omniResponse(engine, kAround[0], s.room.decay * 2.0 + 0.5)));
        INFO("set ", s.room.decay, " measured ", measured);
        CHECK(measured == doctest::Approx(s.room.decay).epsilon(0.06));
    }
}

TEST_CASE("reflections and tail are one room: what is heard is what the balance says") {
    /*  K is the room's whole energy, read off its own reflections; what of it is not heard as a discrete
        reflection comes out of the tail, and the two are scaled to 1 together. What is heard is a little
        under that on both sides, by amounts that are known: the reflections' low-passes take about a
        quarter of theirs, and the tail is as it was listened to -- (1-a)^2 + a^2 of the model's, for a
        diffusion of a. Catches: either gain dropped, the scattered energy fed to the tail
        but left out of its level, and K taken from the wrong window. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    for (const ReverbFrame& s : {preset(5.0, RoomShape::Room, 0.5, 0.42, 0.75, 2.0), hall(),
                                 preset(38.0, RoomShape::Tall, 6.5, 0.35, 0.5, 14.0)}) {
        engine.set(s);
        const ReverbBalance b = engine.balance();
        CHECK(b.roomEnergy > b.mirrorEnergy);
        CHECK(b.tailEnergy == doctest::Approx(b.roomEnergy - b.mirrorEnergy));
        double heard = 0.0;
        for (const Vec3& u : kAround)
            heard += energy(omniResponse(engine, u, s.room.decay * 2.0 + 0.5), 0.0, 100.0) / kAround.size();
        const double a = engine.room().diffusion, asHeard = (1.0 - a) * (1.0 - a) + a * a;
        const double expected = (0.75 * b.mirrorEnergy + asHeard * b.tailEnergy) / b.roomEnergy;
        INFO("heard ", heard, " expected ", expected);
        CHECK(heard == doctest::Approx(expected).epsilon(0.25));
        CHECK(heard < 1.0);
        //  and before the mixing time it is the reflections that are heard: most of their energy is there
        double early = 0.0;
        for (const Vec3& u : kAround)
            early += energy(omniResponse(engine, u, 0.3), 0.0, engine.room().mixingTime) / kAround.size();
        CHECK(early > 0.6 * 0.75 * b.mirrorEnergy / b.roomEnergy);
    }
}

TEST_CASE("the tail takes over from the reflections without a step") {
    /*  In a small room -- where the prototype's first tail came in as a hump at 100 ms -- the energy in
        30 ms windows, a step every 10, over four directions, falls from its peak and never climbs back by
        more than a few decibels. (A hall's first reflections are sparse, single arrivals tens of
        milliseconds apart, and its envelope is not smooth that early by nature.) Catches the tail
        brought in too loud, or later than the reflections it takes over from. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    for (const ReverbFrame& s :
         {preset(4.0, RoomShape::Room, 0.35, 0.5, 0.8, 1.5), preset(5.0, RoomShape::Room, 0.5, 0.42, 0.75, 2.0)}) {
        engine.set(s);
        const int count = 30;
        std::vector<double> env(count, 0.0);
        for (const Vec3& u : kAround) {
            const auto w = omniResponse(engine, u, 0.5);
            for (int i = 0; i < count; ++i) env[static_cast<std::size_t>(i)] += energy(w, i * 0.01, i * 0.01 + 0.03);
        }
        const auto peak = static_cast<int>(std::max_element(env.begin(), env.end()) - env.begin());
        double lowest = env[static_cast<std::size_t>(peak)], worstRise = 0.0;
        for (int i = peak + 1; i < count; ++i) {
            worstRise = std::max(worstRise, 10.0 * std::log10(env[static_cast<std::size_t>(i)] / lowest));
            lowest = std::min(lowest, env[static_cast<std::size_t>(i)]);
        }
        MESSAGE("size ", s.room.size, " peak at ", peak * 10, " ms, worst rise ", worstRise, " dB");
        CHECK(peak * 0.01 < 0.08);
        CHECK(worstRise < 3.0);
    }
}

TEST_CASE("a source farther off excites more room, and the wet sound stays as loud") {
    /*  K is the room's energy against a direct sound of 1: it grows as r squared until the walls hold
        the source in. The reflections' share of it barely moves -- both go as r squared -- so what
        the geometry changes in the wet sound is when things arrive, and the two gains fall together.

        Each distance gets its own engine, because the geometry is built when the room is built and a
        distance that moves afterwards does not rebuild it. That is how a preset arrives: room
        and distance together. */
    double before = 0.0;
    for (const double d : {1.0, 2.0, 4.0, 6.0, 10.0}) {
        ReverbEngine engine;
        engine.prepare(3, kFs);
        ReverbFrame s = hall();
        s.distance = d;
        engine.set(s);
        const ReverbBalance b = engine.balance();
        CHECK(b.roomEnergy > before);
        before = b.roomEnergy;
        const double share = b.mirrorEnergy / b.roomEnergy;
        CHECK(share > 0.1);
        CHECK(share < 0.3);
        //  the two together at unit energy in the model, whatever the distance
        CHECK(b.earlyGain * b.earlyGain * b.roomEnergy == doctest::Approx(1.0));
    }
    ReverbEngine near;
    near.prepare(3, kFs);
    ReverbFrame one = hall();
    one.distance = 1.0;
    near.set(one);
    CHECK(near.balance().roomEnergy == doctest::Approx(before / 100.0).epsilon(0.5));  // r squared, roughly
}

TEST_CASE("a distance that moves on its own costs nothing and moves the dry, not the room") {
    /*  Recomputing every image of every virtual source is 77 us, and an automated distance
        would pay it every control step -- 2.9 % of a core -- to move the wet by under a decibel,
        where the dry moves by 19 dB over the same range. So the geometry is built with the room and
        distance alone touches the dry.

        Catches: distance put back in `roomMoved` (the room's energy moves again, and the cost with
        it), and the dry left as an absolute level rather than a trim on the ratio (it stops tracking
        distance at all). */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    ReverbFrame s = hall();
    s.distance = 6.0;
    s.dryGain = 1.0;  // a trim of 1: the room as it really is
    engine.set(s);
    const ReverbBalance at6 = engine.balance();

    s.distance = 14.0;
    engine.set(s);
    const ReverbBalance at14 = engine.balance();

    //  the room did not move: no rebuild, and every gain it owns is where it was
    CHECK(at14.roomEnergy == doctest::Approx(at6.roomEnergy));
    CHECK(at14.mirrorEnergy == doctest::Approx(at6.mirrorEnergy));
    CHECK(at14.earlyGain == doctest::Approx(at6.earlyGain));
    CHECK(at14.tailGain == doctest::Approx(at6.tailGain));
    CHECK(at14.preDelaySeconds == doctest::Approx(at6.preDelaySeconds));

    //  and the room built at 14 m is a different room, so the geometry still depends on distance --
    //  it is only the recomputation that is dropped, not the dependency
    ReverbEngine built;
    built.prepare(3, kFs);
    ReverbFrame far = hall();
    far.distance = 14.0;
    built.set(far);
    CHECK(built.balance().roomEnergy > at6.roomEnergy * 1.5);
}

TEST_CASE("the level is a level, and the quality switch changes density, not loudness or decay") {
    ReverbEngine engine;
    engine.prepare(3, kFs);
    ReverbFrame s = hall();
    engine.set(s);
    const double reference = energy(omniResponse(engine, kAround[0], 4.0), 0.0, 100.0);
    s.levelDb = -6.0;
    engine.set(s);
    CHECK(energy(omniResponse(engine, kAround[0], 4.0), 0.0, 100.0) ==
          doctest::Approx(reference * std::pow(10.0, -0.6)).epsilon(0.01));
    s.levelDb = 0.0;
    for (const int lines : {8, 32}) {
        s.tailLines = lines;
        engine.set(s);
        const auto w = omniResponse(engine, kAround[0], 4.0);
        INFO("lines ", lines);
        CHECK(10.0 * std::log10(energy(w, 0.0, 100.0) / reference) ==
              doctest::Approx(0.0).epsilon(1.5));  // within 1.5 dB
        CHECK(rt60(mids(w)) == doctest::Approx(1.9).epsilon(0.08));
    }
}

TEST_CASE("the room does not care how the audio is cut, a reset is a fresh start, and what is not a number stays out") {
    const int C = 16, frames = 9000;
    std::vector<float> in(static_cast<std::size_t>(C * frames));
    unsigned seed = 4u;
    for (float& v : in) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 8000.0f;
    }
    in[100 * C + 3] = std::numeric_limits<float>::quiet_NaN();
    in[101 * C + 1] = std::numeric_limits<float>::infinity();
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const auto render = [&](const std::vector<int>& pieces) {
        std::vector<float> out(in.size());
        engine.reset();
        ReverbFrame s = hall();
        int at = 0;
        std::size_t p = 0;
        while (at < frames) {
            int n = std::min(frames - at, pieces[p++ % pieces.size()]);
            while (n > 0) {
                if (at % kReverbHop == 0) {
                    s.levelDb = -3.0 * ((at / kReverbHop) % 3);  // a level that moves, brought in over the hop
                    engine.set(s);
                }
                const int m = std::min(n, kReverbHop - at % kReverbHop);
                engine.process(in.data() + at * C, out.data() + at * C, m);
                at += m;
                n -= m;
            }
        }
        return out;
    };
    const auto whole = render({256}), cut = render({1, 7, 100, 333, 1024});
    for (const float v : whole) REQUIRE(std::isfinite(v));
    CHECK(std::memcmp(whole.data(), cut.data(), whole.size() * sizeof(float)) == 0);
    const auto again = render({512});
    CHECK(std::memcmp(whole.data(), again.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE("a setting handed over only when it changed renders as it would however the audio is cut") {
    //  The hop a level is brought in over can begin anywhere in a caller's pieces; a chunk that straddled
    //  its end took one decision for both sides (found in Echo's review, the same shape here).
    const int C = 16, frames = 4000;
    std::vector<float> in(static_cast<std::size_t>(C * frames));
    unsigned seed = 6u;
    for (float& v : in) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 8000.0f;
    }
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const auto render = [&](int piece) {
        ReverbFrame s = hall();
        engine.set(s);
        engine.reset();
        engine.set(s);
        std::vector<float> out(in.size());
        for (int pos = 0; pos < frames;) {
            if (pos == 2048) {
                s.levelDb = -9.0;
                engine.set(s);
            }
            const int n = std::min(piece, pos < 2048 ? 2048 - pos : frames - pos);
            engine.process(in.data() + pos * C, out.data() + pos * C, n);
            pos += n;
        }
        return out;
    };
    const auto base = render(256);
    CHECK(std::memcmp(base.data(), render(100).data(), base.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(base.data(), render(1).data(), base.size() * sizeof(float)) == 0);
}

// ---- a room that changes size (kTailFadeSeconds) ----------------------------------------

namespace {

/// Steady noise on the bus from one direction: something for a tail to be made of.
std::vector<float> steady(int C, int frames, Vec3 from) {
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(from, 3, y);
    std::vector<float> in(static_cast<std::size_t>(frames) * C);
    std::uint32_t rng = 0x9E3779B9u;
    for (int f = 0; f < frames; ++f) {
        rng = rng * 1664525u + 1013904223u;
        const auto v = static_cast<float>(0.25 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0));
        for (int c = 0; c < C; ++c)
            in[static_cast<std::size_t>(f) * C + c] = v * static_cast<float>(y[static_cast<std::size_t>(c)]);
    }
    return in;
}

/// The output's energy in 30 ms windows, one entry a window.
std::vector<double> envelope(const std::vector<float>& out, int C) {
    const int width = static_cast<int>(0.03 * kFs), frames = static_cast<int>(out.size()) / C;
    std::vector<double> e;
    for (int at = 0; at + width <= frames; at += width) {
        double sum = 0.0;
        for (int f = at; f < at + width; ++f)
            for (int c = 0; c < C; ++c)
                sum += static_cast<double>(out[static_cast<std::size_t>(f) * C + c]) *
                       out[static_cast<std::size_t>(f) * C + c];
        e.push_back(sum);
    }
    return e;
}

/// Render `in` in hops, calling `atHop` before each control step.
std::vector<float> play(ReverbEngine& engine, const std::vector<float>& in, int C,
                        const std::function<void(int, ReverbFrame&)>& atHop, ReverbFrame s) {
    std::vector<float> out(in.size(), 0.0f);
    const int frames = static_cast<int>(in.size()) / C;
    for (int at = 0, hop = 0; at < frames; at += kReverbHop, ++hop) {
        atHop(hop, s);
        engine.set(s);
        const int n = std::min(kReverbHop, frames - at);
        engine.process(in.data() + static_cast<std::size_t>(at) * C, out.data() + static_cast<std::size_t>(at) * C, n);
    }
    return out;
}

}  // namespace

TEST_CASE("a room that changes size does not silence its tail") {
    /*  The lines' lengths and the diffuser come from the volume, so a new size is a new network and
        what was in the old one means nothing in it. Adopting it outright empties the tail, and with a
        source still playing that is an audible hole: measured, the lowest 30 ms window through the
        change sat 32 dB under the level either side. Two networks faded past each other in equal power
        bring it to under 2 dB. Catches: adopt outright (`fresh` always true) and the hole is back. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels(), frames = static_cast<int>(3.0 * kFs);
    const std::vector<float> in = steady(C, frames, fromAzEl(0.4, 0.1));
    const int turn = static_cast<int>(1.5 * kFs / kReverbHop);

    const std::vector<double> e = envelope(play(
                                               engine, in, C,
                                               [=](int hop, ReverbFrame& s) {
                                                   if (hop == turn) s.room.size = 35.0;
                                               },
                                               hall()),
                                           C);

    const auto mean = [&e](int from, int to) {
        double sum = 0.0;
        for (int i = from; i < to; ++i) sum += e[static_cast<std::size_t>(i)];
        return sum / (to - from);
    };
    const int at = static_cast<int>(1.5 / 0.03);
    const double either = 0.5 * (mean(at - 10, at) + mean(at + 20, at + 30));
    double lowest = 1e300;
    for (int i = at; i < at + 20; ++i) lowest = std::min(lowest, e[static_cast<std::size_t>(i)]);
    /*  Measured on this stimulus, lowest 30 ms window through a 20 -> 35 m change: as built -1.25 dB,
        a linear fade of the same two networks -2.66, adopting the layout outright -7.54. The bound sits
        between the first two, so it holds the fade to equal power and not merely to "not a hole". The
        ceiling matters as much and is asserted separately below. */
    INFO("lowest through the change ", 10.0 * std::log10(lowest / either), " dB");
    CHECK(10.0 * std::log10(lowest / either) > -2.0);
}

TEST_CASE("a size dragged does not restart the tail over and over") {
    /*  A knob is dragged, not stepped: the size moves every control step. A fade already running holds
        the newest layout back rather than cutting the one in flight, so the cost is one fade a quarter
        second. Catches: take the newest layout at once, and every fade starts from an empty network
        that the next change replaces before it has filled. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels(), frames = static_cast<int>(3.0 * kFs);
    const std::vector<float> in = steady(C, frames, fromAzEl(-0.9, 0.2));
    const int from = static_cast<int>(1.0 * kFs / kReverbHop), to = static_cast<int>(1.5 * kFs / kReverbHop);

    const std::vector<double> e = envelope(play(
                                               engine, in, C,
                                               [=](int hop, ReverbFrame& s) {
                                                   if (hop >= from && hop <= to)
                                                       s.room.size =
                                                           20.0 + 40.0 * (hop - from) / static_cast<double>(to - from);
                                               },
                                               hall()),
                                           C);

    const auto mean = [&e](int a, int b) {
        double sum = 0.0;
        for (int i = a; i < b; ++i) sum += e[static_cast<std::size_t>(i)];
        return sum / (b - a);
    };
    const int a = static_cast<int>(1.0 / 0.03), b = static_cast<int>(1.5 / 0.03);
    const double either = 0.5 * (mean(a - 10, a) + mean(b + 5, b + 15));
    double lowest = 1e300;
    for (int i = a; i < b + 5; ++i) lowest = std::min(lowest, e[static_cast<std::size_t>(i)]);
    //  measured: as built -0.79 dB, a linear fade -2.48, adopting outright -7.26, cutting the fade in
    //  flight instead of holding the newest layout back -7.06
    INFO("lowest through the drag ", 10.0 * std::log10(lowest / either), " dB");
    CHECK(10.0 * std::log10(lowest / either) > -1.5);
}

TEST_CASE("a room that grew ends as the room it grew into, and grows again") {
    /*  The fade must land somewhere: after it, the room is the new one and not an average of the two.
        Catches: the fade never finishing, or finishing onto the network that was leaving. */
    const int frames = static_cast<int>(4.0 * kFs);
    ReverbFrame big = hall();
    big.room.size = 35.0;

    ReverbEngine grew, always;
    grew.prepare(3, kFs);
    always.prepare(3, kFs);
    const int C = grew.channels();
    const std::vector<float> in = steady(C, frames, fromAzEl(1.1, -0.2));
    const int turn = static_cast<int>(1.0 * kFs / kReverbHop);

    const std::vector<double> a = envelope(play(
                                               grew, in, C,
                                               [=, &big](int hop, ReverbFrame& s) {
                                                   if (hop == turn) s = big;
                                               },
                                               hall()),
                                           C);
    const std::vector<double> b = envelope(play(always, in, C, [](int, ReverbFrame&) {}, big), C);

    //  well after the fade has finished, the two are the same room heard from the same place
    double ga = 0.0, gb = 0.0;
    for (std::size_t i = static_cast<std::size_t>(3.0 / 0.03); i < a.size(); ++i) {
        ga += a[i];
        gb += b[i];
    }
    INFO("grown ", ga, " always ", gb);
    CHECK(std::abs(10.0 * std::log10(ga / gb)) < 1.0);

    /*  And the layout that is live is the new room's, which a level cannot show: the balance holds the
        output at unit energy whatever network is under it, and a stale plan's decay is still set from
        its own lengths. So the lines are asked outright -- and asked again after a second change, which
        is what a fade that never finishes would hold back for ever. */
    for (int k = 0; k < grew.lines(); ++k)
        CHECK(grew.tailDelayOf(k) == doctest::Approx(always.tailDelayOf(k)).epsilon(0.02));

    ReverbFrame small = hall();
    small.room.size = 8.0;
    ReverbEngine tiny;
    tiny.prepare(3, kFs);
    play(tiny, in, C, [](int, ReverbFrame&) {}, small);
    play(
        grew, in, C,
        [&small](int hop, ReverbFrame& s) {
            if (hop == 0) s = small;
        },
        big);
    for (int k = 0; k < grew.lines(); ++k)
        CHECK(grew.tailDelayOf(k) == doctest::Approx(tiny.tailDelayOf(k)).epsilon(0.02));
}

TEST_CASE("a reset plays the room as it is set, not the room it was") {
    /*  There are two networks and the live one is whichever holds the room as it is now. A reset that
        put that back to the first would play the old room's lines with the new room's settings.
        Catches: `live_ = 0` in reset(). */
    ReverbFrame big = hall();
    big.room.size = 35.0;
    const int frames = static_cast<int>(1.0 * kFs);

    ReverbEngine grew, fresh;
    grew.prepare(3, kFs);
    fresh.prepare(3, kFs);
    const int C = grew.channels();
    const std::vector<float> in = steady(C, frames, fromAzEl(0.2, 0.3));

    //  played as the hall, then grown -- so the live network is the second one -- then reset
    std::vector<float> warm(in.size(), 0.0f);
    grew.set(hall());
    grew.process(in.data(), warm.data(), kReverbHop);
    grew.set(big);
    grew.process(in.data(), warm.data(), kReverbHop);
    grew.reset();

    const std::vector<float> after = play(grew, in, C, [](int, ReverbFrame&) {}, big);
    const std::vector<float> never = play(fresh, in, C, [](int, ReverbFrame&) {}, big);
    for (std::size_t i = 0; i < after.size(); ++i) REQUIRE(after[i] == never[i]);
}

TEST_CASE("a size turned slowly still rebuilds the tail: the guard is against the live layout") {
    /*  `lineScale` is linear in size, so a knob turned over a second moves it by about 0.013 a control
        step. A guard that asks "has it moved since the last step" never trips at that rate, and the
        tail keeps the small room's line lengths while the reflections follow the knob all the way --
        one room heard as two. Measured: 20 -> 60 m over half a second trips such a guard 31 times, and
        over one second, 0 times. The guard is against the scale the live layout was built from, so the
        rate of turning cannot matter; the fade is what makes rebuilding that often affordable.
        Catches: compare against the previous step's room, and this fails at every speed but the fastest. */
    ReverbFrame big = hall();
    big.room.size = 60.0;

    for (const double seconds : {0.5, 1.0, 3.0}) {
        ReverbEngine turned, straight;
        turned.prepare(3, kFs);
        straight.prepare(3, kFs);
        const int C = turned.channels(), hops = static_cast<int>(seconds * kFs / kReverbHop);
        const std::vector<float> in = steady(C, (hops + 40) * kReverbHop, fromAzEl(0.7, 0.0));

        play(
            turned, in, C,
            [=](int hop, ReverbFrame& s) {
                s.room.size = 20.0 + 40.0 * std::min(1.0, hop / static_cast<double>(hops));
            },
            hall());
        play(straight, in, C, [](int, ReverbFrame&) {}, big);

        INFO("turned over ", seconds, " s");
        for (int k = 0; k < turned.lines(); ++k)
            CHECK(turned.tailDelayOf(k) == doctest::Approx(straight.tailDelayOf(k)).epsilon(0.04));
    }
}

TEST_CASE("a size change is not heard as a swell either: each network keeps its own normalisation") {
    /*  The test above reads the floor, and a floor metric cannot see a level going up. It does here:
        the tail's gain is normalised by what the live network makes of a unit impulse, and a longer
        line holds less energy for the same decay, so playing the network that is leaving through the
        gain of the one arriving is a level error that grows with the change -- measured +2.34 dB at
        20 -> 35 m, a swell exactly where the fade exists to hide a discontinuity. Each network is
        played through its own normalisation, and the ceiling comes back to +0.40 dB, against a
        measurement floor of +0.22 on this stimulus with no change at all.

        Catches: leavingRatio_ forced to 1, and the swell returns at every size but the smallest. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels(), frames = static_cast<int>(3.0 * kFs);
    const std::vector<float> in = steady(C, frames, fromAzEl(0.4, 0.1));
    const int turn = static_cast<int>(1.5 * kFs / kReverbHop), at = static_cast<int>(1.5 / 0.03);

    for (const double size : {20.0, 21.0, 22.0, 25.0, 35.0}) {
        const std::vector<double> e = envelope(play(
                                                   engine, in, C,
                                                   [=](int hop, ReverbFrame& s) {
                                                       if (hop == turn) s.room.size = size;
                                                   },
                                                   hall()),
                                               C);
        const auto mean = [&e](int a, int b) {
            double sum = 0.0;
            for (int i = a; i < b; ++i) sum += e[static_cast<std::size_t>(i)];
            return sum / (b - a);
        };
        const double either = 0.5 * (mean(at - 10, at) + mean(at + 20, at + 30));
        double high = 0.0, low = 1e300;
        for (int i = at; i < at + 20; ++i) {
            high = std::max(high, e[static_cast<std::size_t>(i)]);
            low = std::min(low, e[static_cast<std::size_t>(i)]);
        }
        MESSAGE("20 -> " << size << " m: highest " << 10.0 * std::log10(high / either) << " dB, lowest "
                         << 10.0 * std::log10(low / either) << " dB");
        CHECK(10.0 * std::log10(high / either) < 1.0);
    }
}

TEST_CASE("the direct is never louder than what came in, and the ratio to the room is kept") {
    /*  A hall at 0.5 m put the dry at 8.57 -- 18.7 dB over the input. Catches: no cap (the dry
        over 1), the cap on the dry alone (the room left where it was, the ratio lost), and the cap
        applied with the dry trimmed away (the room turned down for a dry nobody hears). */
    const auto gains = [](double distance, double dryTrim) {
        ReverbFrame s = hall();
        s.distance = distance;
        s.dryGain = dryTrim;
        ReverbEngine e;
        e.prepare(3, kFs);
        e.set(s);
        //  the room's level after the cap: the impulse response's energy
        const auto omni = omniResponse(e, Vec3{1.0, 0.0, 0.0}, 1.0);
        double wet = 0.0;  // the dry is the impulse itself, at the first sample: left out
        for (std::size_t i = 1; i < omni.size(); ++i) wet += static_cast<double>(omni[i]) * omni[i];
        return std::pair<double, double>{e.balance().dryGain, wet};
    };
    const auto [farDry, farWet] = gains(6.0, 1.0);
    const auto [nearDry, nearWet] = gains(0.5, 1.0);
    const auto [offDry, offWet] = gains(0.5, 0.0);
    CHECK(farDry < 1.0);                     // at its own distance the hall is below the input
    CHECK(nearDry == doctest::Approx(1.0));  // close up, the dry is held at the input
    //  the room came down with it: against the room with the dry trimmed away, by the dry's excess
    const double uncapped = gains(0.5, 1e-9).first * 1e9;  // what the dry would have been
    CHECK(uncapped > 2.0);
    CHECK(10.0 * std::log10(offWet / nearWet) == doctest::Approx(20.0 * std::log10(uncapped)).epsilon(0.02));
    CHECK(offDry == 0.0);
}
