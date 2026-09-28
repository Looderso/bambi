// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "bambi/math/sh.hpp"
#include "bambi/reverb/early.hpp"
#include "bambi/reverb/engine.hpp"
#include "doctest.h"

/*  Reverb's send and return.
 *
 *  The send decides what of the field enters the room, the return where what the room made is put
 *  back. The engine already reduces the field to directions, so both are gains -- twelve virtual
 *  sources and K lines -- except the reflections' half of the return, which goes back over the whole
 *  bus and is the shared projection. Each case here names the mutation it catches.
 */
namespace {

using namespace bambi;

constexpr double kFs = 48000.0;

ReverbFrame hall() {
    ReverbFrame s;
    s.room = {20.0, RoomShape::Hall, 1.9, 0.5, 0.6};
    s.distance = 6.0;
    s.lowCutHz = 20.0;
    s.highCutHz = 20000.0;
    return s;
}

Region spot(double yaw, double size, double softness = 0.0) {
    Region r;
    r.kind = RegionKind::Spot;
    r.yaw = yaw;
    r.size = size;
    r.softness = softness;
    return r;
}

/// The whole bus of the room's answer to an impulse from `from`, `seconds` long, in hops.
std::vector<float> render(ReverbEngine& engine, const ReverbFrame& s, Vec3 from, double seconds) {
    const int C = engine.channels(), frames = static_cast<int>(seconds * kFs);
    std::vector<float> in(static_cast<std::size_t>(frames) * C, 0.0f), out(in.size(), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(from, 3, y);
    for (int c = 0; c < C; ++c) in[static_cast<std::size_t>(c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    engine.reset();
    engine.set(s);
    for (int at = 0; at < frames; at += kReverbHop) {
        const int n = std::min(kReverbHop, frames - at);
        engine.process(in.data() + static_cast<std::size_t>(at) * C, out.data() + static_cast<std::size_t>(at) * C, n);
    }
    return out;
}

double energy(const std::vector<float>& bus, int C, double fromSeconds, double toSeconds) {
    double e = 0.0;
    const int a = static_cast<int>(fromSeconds * kFs),
              b = std::min(static_cast<int>(toSeconds * kFs), static_cast<int>(bus.size()) / C);
    for (int f = a; f < b; ++f)
        for (int c = 0; c < C; ++c)
            e += static_cast<double>(bus[static_cast<std::size_t>(f) * C + c]) *
                 bus[static_cast<std::size_t>(f) * C + c];
    return e;
}

/// What the bus reads in one direction, squared and summed: where the room was put back.
double energyToward(const std::vector<float>& bus, int C, Vec3 direction, double fromSeconds, double toSeconds) {
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(direction, 3, y);
    double e = 0.0;
    const int a = static_cast<int>(fromSeconds * kFs),
              b = std::min(static_cast<int>(toSeconds * kFs), static_cast<int>(bus.size()) / C);
    for (int f = a; f < b; ++f) {
        double v = 0.0;
        for (int c = 0; c < C; ++c) v += y[static_cast<std::size_t>(c)] * bus[static_cast<std::size_t>(f) * C + c];
        e += v * v;
    }
    return e;
}

const Vec3 kFront{1.0, 0.0, 0.0}, kBack{-1.0, 0.0, 0.0};

}  // namespace

TEST_CASE("a region that is everywhere at an amount of one is not there at all") {
    //  Catches: any gain applied where it should not be, or a projection run for a plain region.
    ReverbEngine a, b;
    a.prepare(3, kFs);
    b.prepare(3, kFs);
    const std::vector<float> plain = render(a, hall(), kFront, 0.5);
    ReverbFrame s = hall();
    s.send = Region{};
    s.returnRegion = Region{};
    const std::vector<float> regions = render(b, s, kFront, 0.5);
    REQUIRE(plain.size() == regions.size());
    for (std::size_t i = 0; i < plain.size(); ++i) REQUIRE(plain[i] == regions[i]);
}

TEST_CASE("a send hears what is inside it and not what is behind it") {
    //  Catches: the send gains never reaching the virtual sources, or never reaching the lines --
    //  each half is asserted on its own, in its own window.
    ReverbFrame s = hall();
    s.send = spot(0.0, 50.0 * kDeg2Rad, 10.0 * kDeg2Rad);
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const std::vector<float> front = render(engine, s, kFront, 1.0);
    const std::vector<float> back = render(engine, s, kBack, 1.0);
    const int C = engine.channels();

    const double tm = engine.room().mixingTime;
    //  the reflections' half: everything before the mixing time is over
    CHECK(energy(back, C, 0.0, tm) < 0.1 * energy(front, C, 0.0, tm));
    //  the tail's half: well after it, where only the lines are left
    CHECK(energy(back, C, 0.3, 1.0) < 0.2 * energy(front, C, 0.3, 1.0));

    //  and with no send at all the two directions are alike: the room has no front
    ReverbFrame open = hall();
    const std::vector<float> f2 = render(engine, open, kFront, 1.0), b2 = render(engine, open, kBack, 1.0);
    CHECK(energy(b2, C, 0.0, 1.0) == doctest::Approx(energy(f2, C, 0.0, 1.0)).epsilon(0.35));
}

TEST_CASE("a return puts the room back where it says, in the reflections and in the tail") {
    //  Catches: the projection dropped from the reflections (the early window goes even again), or
    //  the lines' return gain dropped (the late window does).
    ReverbFrame s = hall();
    s.returnRegion = spot(0.0, 50.0 * kDeg2Rad, 10.0 * kDeg2Rad);
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const std::vector<float> aimed = render(engine, s, kFront, 1.0);
    const std::vector<float> open = render(engine, hall(), kFront, 1.0);
    const int C = engine.channels();
    const double tm = engine.room().mixingTime;

    const double earlyAimed =
        energyToward(aimed, C, kFront, 0.0, tm) / std::max(energyToward(aimed, C, kBack, 0.0, tm), 1e-30);
    const double earlyOpen =
        energyToward(open, C, kFront, 0.0, tm) / std::max(energyToward(open, C, kBack, 0.0, tm), 1e-30);
    CHECK(earlyAimed > 3.0 * earlyOpen);

    const double lateAimed =
        energyToward(aimed, C, kFront, 0.3, 1.0) / std::max(energyToward(aimed, C, kBack, 0.3, 1.0), 1e-30);
    const double lateOpen =
        energyToward(open, C, kFront, 0.3, 1.0) / std::max(energyToward(open, C, kBack, 0.3, 1.0), 1e-30);
    CHECK(lateAimed > 3.0 * lateOpen);
}

TEST_CASE("a dots return and a dots send both shape the room: every kind is projected in the step") {
    /*  A dots return can pass the field whole if its matrix is never built, and this case pins
     *  that. Catches: the dense kinds unbuilt again, and the return renders as no region at all. */
    ReverbEngine a, b;
    a.prepare(3, kFs);
    b.prepare(3, kFs);
    Region dots;
    dots.kind = RegionKind::Dots;
    dots.dots = 6;
    dots.dotSize = 20.0 * kDeg2Rad;

    ReverbFrame s = hall();
    s.returnRegion = dots;
    const std::vector<float> withDots = render(a, s, kFront, 0.4);
    const std::vector<float> plain = render(b, hall(), kFront, 0.4);
    CHECK(energy(withDots, a.channels(), 0.0, 0.4) < 0.7 * energy(plain, b.channels(), 0.0, 0.4));

    ReverbFrame sent = hall();
    sent.send = dots;
    const std::vector<float> withSend = render(a, sent, kFront, 0.4);
    CHECK(energy(withSend, a.channels(), 0.0, 0.4) < 0.7 * energy(plain, b.channels(), 0.0, 0.4));
}

TEST_CASE("a return that is the outside of everywhere silences the room, in both halves") {
    /*  Catches: the shortcut that skips the projection asking only whether the operator is the
     *  identity. "Everywhere, outside" is the identity and passes nothing, so the reflections would
     *  come through whole -- and the lines, forced to a gain of 1 beside them. */
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels();
    ReverbFrame s = hall();
    s.returnRegion.side = RegionSide::Outside;
    CHECK(energy(render(engine, s, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.0));

    //  and the send, which never had a projection to be wrong about, does the same
    ReverbFrame t = hall();
    t.send.side = RegionSide::Outside;
    CHECK(energy(render(engine, t, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.0));
}

TEST_CASE("the amounts are gains, and each scales the half it belongs to") {
    //  Catches: an amount folded into the wrong gain, or left out of one of the two return paths.
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels();
    const double full = energy(render(engine, hall(), kFront, 0.5), C, 0.0, 0.5);

    ReverbFrame quiet = hall();
    quiet.returnAmount = 0.5;
    CHECK(energy(render(engine, quiet, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.25 * full).epsilon(1e-3));

    ReverbFrame halfSend = hall();
    halfSend.sendAmount = 0.5;
    CHECK(energy(render(engine, halfSend, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.25 * full).epsilon(1e-3));

    ReverbFrame shut = hall();
    shut.sendAmount = 0.0;
    CHECK(energy(render(engine, shut, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.0));
    ReverbFrame muted = hall();
    muted.returnAmount = 0.0;
    CHECK(energy(render(engine, muted, kFront, 0.5), C, 0.0, 0.5) == doctest::Approx(0.0));
}

TEST_CASE("a send gain that changes is reached over a hop, not stepped") {
    /*  Catches: the crossfade of the send gains removed, so a region that moves steps the input of
     *  twelve delay lines. Read where the fade lives, and at the one place a step is unmistakable:
     *  the first reflection to arrive after the gains open. Stepped, it arrives at its own full
     *  weight; faded, at a hop's first slice of it -- a factor of the hop apart. Further in the two
     *  converge, which is why the test looks at the first arrival and not at the level. */
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(deriveRoom({20.0, RoomShape::Hall, 1.9, 0.5, 0.6}), 6.0);
    early.reset();

    const int C = numChannels(3);
    std::array<float, kVirtualSources> shut{}, open{};
    shut.fill(0.0f);
    open.fill(1.0f);
    std::vector<float> in(static_cast<std::size_t>(kReverbHop) * C, 0.0f), field(in.size()),
        scattered(static_cast<std::size_t>(kReverbHop) * kVirtualSources);
    for (int f = 0; f < kReverbHop; ++f) in[static_cast<std::size_t>(f) * C] = 1.0f;  // DC on the omni channel

    early.setSourceGains(shut, kReverbHop);  // after a reset the gains snap: nothing goes in at all
    early.process(in.data(), field.data(), scattered.data(), kReverbHop);
    for (float v : field) REQUIRE(v == 0.0f);

    early.setSourceGains(open, kReverbHop);
    double first = 0.0, most = 0.0;
    for (int h = 0; h < 4; ++h) {
        early.process(in.data(), field.data(), scattered.data(), kReverbHop);
        for (int f = 0; f < kReverbHop; ++f) {
            const double v = std::abs(static_cast<double>(field[static_cast<std::size_t>(f) * C]));
            if (first == 0.0 && v > 1e-9) first = v;
            most = std::max(most, v);
        }
    }
    REQUIRE(most > 0.0);
    CHECK(first < 0.02 * most);
}

TEST_CASE("regions that move do not care how the audio is cut, and a reset is a fresh start") {
    /*  Catches: a crossfade read from a chunk-relative position rather than the absolute one, or a
     *  reset that leaves the gains half faded. */
    const int hops = 12, frames = hops * kReverbHop;
    ReverbEngine engine;
    engine.prepare(3, kFs);
    const int C = engine.channels();
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(Vec3{0.3, -0.5, 0.8}, 3, y);
    std::vector<float> in(static_cast<std::size_t>(frames) * C);
    std::uint32_t rng = 12345;
    for (int f = 0; f < frames; ++f) {
        rng = rng * 1664525u + 1013904223u;
        const auto v = static_cast<float>(static_cast<double>(rng >> 8) / 8388608.0 - 1.0);
        for (int c = 0; c < C; ++c)
            in[static_cast<std::size_t>(f) * C + c] = v * static_cast<float>(y[static_cast<std::size_t>(c)]);
    }

    auto run = [&](int cut, bool fresh) {
        std::vector<float> out(in.size(), 0.0f);
        engine.reset();
        ReverbFrame s = hall();
        for (int h = 0; h < hops; ++h) {
            s.send = spot(h * 0.4, (40.0 + 4.0 * h) * kDeg2Rad, 8.0 * kDeg2Rad);
            s.returnRegion = spot(-h * 0.3, 70.0 * kDeg2Rad, 12.0 * kDeg2Rad);
            s.sendAmount = 0.4 + 0.05 * h;
            s.returnAmount = 1.0 - 0.02 * h;
            engine.set(s);
            for (int at = 0; at < kReverbHop;) {
                const int n = std::min(cut, kReverbHop - at);
                const int f = h * kReverbHop + at;
                engine.process(in.data() + static_cast<std::size_t>(f) * C,
                               out.data() + static_cast<std::size_t>(f) * C, n);
                at += n;
            }
        }
        (void)fresh;
        return out;
    };

    const std::vector<float> whole = run(kReverbHop, true);
    for (int cut : {1, 7, 64, 100}) {
        const std::vector<float> cutUp = run(cut, false);
        for (std::size_t i = 0; i < whole.size(); ++i) REQUIRE(cutUp[i] == whole[i]);
    }
    //  and the engine that was reset is the engine that was never played
    const std::vector<float> again = run(kReverbHop, true);
    for (std::size_t i = 0; i < whole.size(); ++i) REQUIRE(again[i] == whole[i]);
}
