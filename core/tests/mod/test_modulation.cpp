// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The modulation engine is where every other module finally connects, so these tests are
//  mostly about the joins rather than the arithmetic: that a rate means what it says, that
//  silence parks the source, that a bounce repeats.

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "bambi/encode/params.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kDt = 1.0 / 187.5;  ///< one hop of 256 at 48 kHz -- the real control rate

PluginState stateWithCell(MatrixTab tab, int column, ParamId target, double depth) {
    PluginState st{encodeParams()};
    st.matrix.push_back(MatrixCell{tab, column, target, depth});
    return st;
}

void setParam(PluginState& st, ParamId id, double v) {
    st.params[static_cast<std::size_t>(id)] = static_cast<float>(v);
}

}  // namespace

// ------------------------------------------------------------------------------- sources

TEST_CASE("LFO shapes stay inside the bipolar range and have the right zero crossings") {
    Lfo lfo;
    lfo.prepare(48000.0);
    Transport tp;
    tp.playing = true;  // a free LFO set to restart runs only while playing
    for (LfoShape shape : {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw}) {
        lfo.reset();
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 2000; ++i) {
            const double v = lfo.process(kDt, tp, false, false, 1.0, 1.0, shape, 0.0);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        INFO("shape ", static_cast<int>(shape), " range ", lo, "..", hi);
        CHECK(lo >= -1.0000001);
        CHECK(hi <= 1.0000001);
        CHECK(lo < -0.9);  // actually uses the range rather than hovering near zero
        CHECK(hi > 0.9);
    }
}

/*  Determinism, the constraint that decides whether a source may exist at all. A synced LFO
 *  must be a pure function of transport position -- not of how long the plugin has been
 *  running -- or an offline bounce that starts mid-timeline lands on a different phase than
 *  the realtime pass did. */
TEST_CASE("a synced LFO depends only on transport position, not on history") {
    Lfo a, b;
    a.prepare(48000.0);
    b.prepare(48000.0);

    Transport tp;
    tp.playing = true;
    tp.bpm = 120.0;

    // `a` runs from the start of the timeline; `b` is dropped in at the same instant.
    double va = 0.0;
    for (int i = 0; i < 500; ++i) {
        tp.ppq = i * 0.01;
        va = a.process(kDt, tp, true, false, 1.0, 1.0, LfoShape::Sine, 0.0);
    }
    const double vb = b.process(kDt, tp, true, false, 1.0, 1.0, LfoShape::Sine, 0.0);
    INFO("continuous ", va, "  dropped in ", vb);
    CHECK(va == doctest::Approx(vb).epsilon(1e-12));
}

TEST_CASE("sample and hold is seeded, so a bounce repeats the take") {
    auto run = [](std::uint32_t seed) {
        Lfo lfo;
        lfo.prepare(48000.0);
        lfo.setSeed(seed);
        Transport tp;
        tp.playing = true;  // a free LFO set to restart runs only while playing
        std::vector<double> out;
        for (int i = 0; i < 400; ++i)
            out.push_back(lfo.process(kDt, tp, false, false, 4.0, 1.0, LfoShape::SampleHold, 0.0));
        return out;
    };
    const auto a = run(12345), b = run(12345), c = run(999);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);  // bit-identical
    CHECK(a != c);                                                     // and seed matters

    int steps = 0;
    for (std::size_t i = 1; i < a.size(); ++i)
        if (a[i] != a[i - 1]) ++steps;
    INFO("held steps in 400 control steps at 4 Hz");
    CHECK(steps > 3);   // it does latch periodically
    CHECK(steps < 40);  // and it holds between latches rather than running free
}

/*  One polarity knob, whose ceiling is pinned at 1 and whose floor sweeps from 0 to -1. A
 *  bounded, one-directional destination -- motion.displace among them -- can then be swept by a
 *  wave rather than folded in half onto itself, without the source ever leaving [-1, 1], which is
 *  what independent amplitude and offset could not promise.
 *
 *  Catches: polarity ignored (the two engines agree); the map applied to the raw [-1, 1] shape
 *  rather than to it converted into [0, 1] first (polarity 1 would reach -3); the ceiling sweeping
 *  instead of the floor (unipolar's top would not sit at exactly 1).
 */
TEST_CASE("an LFO's polarity opens its floor and never its ceiling, and 1 is the bare bipolar shape") {
    PluginState st{encodeParams()};
    setParam(st, EncoderParam::Lfo1Rate, 1.0);
    setParam(st, EncoderParam::Lfo1Sync, 0.0);  // free-running
    setParam(st, EncoderParam::Lfo1Shape, static_cast<double>(LfoShape::Sine));

    PluginState uni = st;
    setParam(uni, EncoderParam::Lfo1Polarity, 0.0);  // unipolar

    ModulationEngine bipolar, unipolar;
    bipolar.useManifest(encodeMod());
    unipolar.useManifest(encodeMod());
    bipolar.prepare(48000.0);
    unipolar.prepare(48000.0);
    bipolar.setState(st);  // polarity 1 by default: an LFO rests bipolar
    unipolar.setState(uni);

    const std::vector<double> feats(6, 0.0);
    Transport tp;
    tp.playing = true;

    double lo = 1e9, hi = -1e9, bipolarLo = 1e9, bipolarHi = -1e9;
    for (int i = 0; i < 2000; ++i) {
        bipolar.process(feats, {}, tp, kDt);
        unipolar.process(feats, {}, tp, kDt);
        //  the same sine either way, so one is exactly the affine map of the other
        CHECK(unipolar.sourceValue(12) == doctest::Approx(0.5 * (bipolar.sourceValue(12) + 1.0)));
        lo = std::min(lo, unipolar.sourceValue(12));
        hi = std::max(hi, unipolar.sourceValue(12));
        bipolarLo = std::min(bipolarLo, bipolar.sourceValue(12));
        bipolarHi = std::max(bipolarHi, bipolar.sourceValue(12));
    }
    //  unipolar never goes negative, and still uses its range rather than hovering near a centre
    CHECK(lo >= -1e-9);
    CHECK(hi <= 1.0 + 1e-9);
    CHECK(lo < 0.1);
    CHECK(hi > 0.9);
    //  and polarity 1 is the bare shape: [-1, 1], the ceiling in the same place as unipolar's
    CHECK(bipolarLo < -0.9);
    CHECK(bipolarLo >= -1.0 - 1e-9);
    CHECK(bipolarHi == doctest::Approx(hi));
}

/*  `ease(1, k) == 1` for every k is the whole property the curve rests on: a bowed stage still
 *  arrives at exactly its labelled time, so the millisecond printed on the tile stays true.
 *  Catches: a real exponential, which never arrives. */
TEST_CASE("a curved stage still reaches its target at exactly the labelled time") {
    for (const double curve : {-1.0, -0.5, -0.04, 0.0, 0.04, 0.5, 1.0}) {
        CHECK(ease(1.0, curve) == doctest::Approx(1.0));
        CHECK(ease(0.0, curve) == doctest::Approx(0.0));
    }
    //  and it bows the right way: positive is fast-then-slow, so half way through is already past half
    CHECK(ease(0.5, 0.5) > 0.5);
    CHECK(ease(0.5, -0.5) < 0.5);
    CHECK(ease(0.5, 0.0) == doctest::Approx(0.5));

    //  the stage time is unchanged by the curve, which is the reason for all of it
    for (const double curve : {-1.0, 0.0, 1.0}) {
        Envelope e;
        Envelope::Params p;
        p.attackS = 0.050;
        p.decayS = 0.100;
        p.sustain = 0.5;
        p.releaseS = 0.080;
        p.attackCurve = curve;
        /*  When the stage ends, not when the value first rounds to its target: a fast-then-slow
            bow is within a thousandth of 1 long before it arrives, and the claim is about the
            stage's length. The peak is where it stops rising. */
        const double dt = 0.001;
        double t = 0.0, tPeak = -1.0, last = -1.0;
        for (int i = 0; i < 100; ++i, t += dt) {
            const double v = e.process(dt, true, p);
            if (tPeak < 0.0 && v <= last) tPeak = t;
            last = v;
        }
        CHECK(tPeak > 0.045);
        CHECK(tPeak < 0.055);
    }
}

/*  The times printed on an envelope have to be the times it takes, which is why the stages
 *  ramp linearly instead of approaching exponentially. */
TEST_CASE("envelope stage times are the times on the label") {
    Envelope e;
    Envelope::Params p;
    p.attackS = 0.050;
    p.decayS = 0.100;
    p.sustain = 0.5;
    p.releaseS = 0.080;

    const double dt = 0.001;
    double t = 0.0, tPeak = -1.0;
    for (int i = 0; i < 100; ++i, t += dt) {
        e.process(dt, true, p);
        if (tPeak < 0.0 && e.value() >= 0.999) tPeak = t;
    }
    /*  Explicit bounds, not doctest::Approx. Approx compares against epsilon * (scale + max), with
        scale defaulting to 1, so on a value of 0.05 seconds `.epsilon(0.05)` is a tolerance of
        0.0525 -- wider than the value itself, and wider than the whole loop. This assertion passed
        with the attack running 1.8x slower than its label, which is precisely what it exists to
        catch. Anything measured in units well below 1 wants a bound or a ratio. */
    INFO("reached full at ", tPeak, "s, expected 0.050");
    CHECK(tPeak > 0.0475);
    CHECK(tPeak < 0.0525);

    for (int i = 0; i < 300; ++i) e.process(dt, true, p);
    CHECK(e.value() > 0.49);  // sustain
    CHECK(e.value() < 0.51);

    double tZero = -1.0;
    t = 0.0;
    for (int i = 0; i < 200; ++i, t += dt) {
        e.process(dt, false, p);
        if (tZero < 0.0 && e.value() <= 0.0) tZero = t;
    }
    INFO("released at ", tZero, "s, expected 0.5 * 0.080 = 0.040 from sustain");
    CHECK(tZero > 0.036);
    CHECK(tZero < 0.044);
}

TEST_CASE("an untriggered envelope rests at zero, not at minus one") {
    //  The documented departure from "generators are bipolar". A bipolar envelope would
    //  shove every destination it is patched to against a rail while nothing is happening.
    Envelope e;
    Envelope::Params p;
    for (int i = 0; i < 500; ++i) CHECK(e.process(kDt, false, p) == doctest::Approx(0.0));
}

// -------------------------------------------------------------------------- the equation

TEST_CASE("contribution is source x amount x depth") {
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::RenderWidth, 0.5);
    setParam(st, EncoderParam::RenderWidth, 0.0);
    setParam(st, EncoderParam::ModAmountLevel, 0.5);
    setParam(st, EncoderParam::ModGlobalAmount, 1.0);

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);

    const std::vector<double> feats{1.0, 0, 0, 0, 0, 0};  // level = 1
    Transport tp;
    for (int i = 0; i < 2000; ++i) m.process(feats, {}, tp, kDt);

    //  width spans 0..180, so 1.0 x 0.5 x 0.5 = 0.25 of full span = 45 deg.
    INFO("width ", m.destination(EncoderParam::RenderWidth));
    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(45.0).epsilon(0.01));
}

TEST_CASE("the global amount scales every cell at once") {
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::RenderWidth, 1.0);
    setParam(st, EncoderParam::RenderWidth, 0.0);
    setParam(st, EncoderParam::ModGlobalAmount, 0.25);

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    const std::vector<double> feats{1.0, 0, 0, 0, 0, 0};
    Transport tp;
    for (int i = 0; i < 2000; ++i) m.process(feats, {}, tp, kDt);
    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(45.0).epsilon(0.01));
}

TEST_CASE("a destination clamps to its range and says when it is railing") {
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::RenderWidth, 1.0);
    setParam(st, EncoderParam::RenderWidth, 150.0);  // base already near the top of 0..180

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    const std::vector<double> feats{1.0, 0, 0, 0, 0, 0};
    Transport tp;
    for (int i = 0; i < 2000; ++i) m.process(feats, {}, tp, kDt);

    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(180.0).epsilon(0.001));
    CHECK(m.railing(EncoderParam::RenderWidth));  // the UI has to be able to show this
}

TEST_CASE("an absent sidechain reads zero rather than holding its last value") {
    PluginState st = stateWithCell(MatrixTab::Sidechain, 0, EncoderParam::RenderWidth, 1.0);
    setParam(st, EncoderParam::RenderWidth, 0.0);

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;

    const std::vector<double> sc{1.0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 2000; ++i) m.process({}, sc, tp, kDt);
    REQUIRE(m.destination(EncoderParam::RenderWidth) > 100.0);

    for (int i = 0; i < 4000; ++i) m.process({}, {}, tp, kDt);  // sidechain goes away
    INFO("width after the sidechain disconnects: ", m.destination(EncoderParam::RenderWidth));
    CHECK(m.destination(EncoderParam::RenderWidth) < 1.0);
}

// --------------------------------------------------------------------------- destinations

TEST_CASE("an angle destination is bounded in degrees per second, not merely smoothed") {
    //  A quarter of the span: a whole one is a whole turn, which is where it started.
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::TransformYaw, 0.25);
    setParam(st, EncoderParam::TransformYaw, 0.0);

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;

    /*  Settle at zero modulation first. The engine snaps a destination to its target on the
     *  very first step -- correctly, since there is nothing to slew from -- so measuring
     *  across that step measures the snap instead of the limiter. An earlier version of this
     *  test did exactly that, skipped the one step where the jump happened, and passed
     *  happily with the limit loosened 1000x. */
    const std::vector<double> off{0.0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 200; ++i) m.process(off, {}, tp, kDt);
    REQUIRE(m.destination(EncoderParam::TransformYaw) == doctest::Approx(0.0).epsilon(1e-6));

    const std::vector<double> on{1.0, 0, 0, 0, 0, 0};
    double prev = m.destination(EncoderParam::TransformYaw);
    double worst = 0.0;
    for (int i = 0; i < 400; ++i) {
        m.process(on, {}, tp, kDt);
        const double v = m.destination(EncoderParam::TransformYaw);
        worst = std::max(worst, std::abs(v - prev) / kDt);
        prev = v;
    }
    CHECK(worst > 1.0);  // it did move, so the bound below is measuring something real
    INFO("peak angular velocity ", worst, " deg/s, limit ", destinationSmoothing(EncoderParam::TransformYaw));
    CHECK(worst <= destinationSmoothing(EncoderParam::TransformYaw) * 1.001);
}

TEST_CASE("a parameter with no modulation route is not a destination") {
    CHECK(destinationKind(EncoderParam::MotionMode) == DestKind::NotModulatable);
    CHECK(destinationKind(EncoderParam::Lfo1Shape) == DestKind::NotModulatable);
    CHECK(destinationKind(EncoderParam::ModAmountLevel) == DestKind::NotModulatable);
    CHECK(destinationKind(EncoderParam::MotionSpeed) == DestKind::Rate);
}

// -------------------------------------------------------------------------- motion clock

/*  What arc-length parameterisation was FOR. Speed is degrees per second of travel on the
 *  sphere, and it has to mean that on a short path and a long one alike -- otherwise the
 *  same knob means a different velocity on every trajectory and the premise of the plugin
 *  (loudness driving speed) is not expressible. */
TEST_CASE("speed is real angular velocity, whatever the path length") {
    for (double lengthRad : {0.5, kPi, 2.0 * kPi, 12.0}) {
        MotionClock c;
        const double speedDegPerSec = 90.0;
        const int steps = 1000;
        const double dt = 1.0 / steps;
        for (int i = 0; i < steps; ++i) c.advance(speedDegPerSec, lengthRad, dt);

        //  One second at 90 deg/s is a quarter turn of arc, i.e. 90/deg of the path length.
        const double travelledRad = c.phase() * lengthRad;
        INFO("length ", lengthRad, " travelled ", travelledRad * kRad2Deg, " deg");
        CHECK(travelledRad * kRad2Deg == doctest::Approx(90.0).epsilon(1e-9));
    }
}

/*  Regression for a bug that shipped in the prototype and read as "offset does too much":
 *  adding the offset to the resulting arc length and clamping parks the source against an
 *  extreme for most of every lap. It belongs on the phase, before the mode mapping. */
TEST_CASE("displace shifts the phase rather than clamping the arc length") {
    MotionClock c;
    c.reset(0.0);
    const double displaced = c.sAt(0.25, MovementMode::Wrap, true);
    CHECK(displaced == doctest::Approx(0.25).epsilon(1e-9));

    //  And it must keep travelling rather than parking: sample a whole lap and check the
    //  source visits the full range instead of sitting at a rail.
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 1000; ++i) {
        c.advance(360.0, 2.0 * kPi, 1.0 / 1000.0);
        const double s = c.sAt(0.6, MovementMode::Wrap, true);
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    INFO("s ranged ", lo, "..", hi, " over one lap");
    CHECK(lo < 0.05);
    CHECK(hi > 0.95);
}

// ------------------------------------------------------------------------------ the whole

/*  The premise: silence parks the source; sound moves it. If this test ever fails the
 *  plugin does not do the one thing it exists to do. */
TEST_CASE("silence parks the source and level moves it") {
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::MotionSpeed, 1.0);
    setParam(st, EncoderParam::MotionSpeed, 0.0);  // no constant motion: all of it is the signal

    TrajectoryState ts;
    ts.kind = TrajectoryKind::Parametric;
    ts.generator = GeneratorType::Orbit;
    generatorDefaults(ts.generator, ts.genParams);
    Trajectory traj;
    traj.build(ts);
    REQUIRE(traj.lengthRad() > 0.1);

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    MotionClock clock;
    Transport tp;

    auto runFor = [&](double level, int steps) {
        const std::vector<double> f{level, 0, 0, 0, 0, 0};
        for (int i = 0; i < steps; ++i) {
            m.process(f, {}, tp, kDt);
            clock.advance(m.destination(EncoderParam::MotionSpeed), traj.lengthRad(), kDt);
        }
        return clock.phase();
    };

    const double afterSilence = runFor(0.0, 1000);
    INFO("phase after 5 s of silence: ", afterSilence);
    CHECK(std::abs(afterSilence) < 1e-9);  // did not move at all

    const double afterSound = runFor(1.0, 1000);
    INFO("phase after 5 s of full level: ", afterSound);
    CHECK(afterSound > 0.1);  // and moved once there was signal
}

TEST_CASE("the whole chain is deterministic — the offline bounce requirement") {
    auto render = [] {
        PluginState st = stateWithCell(MatrixTab::Features, 1, EncoderParam::MotionSpeed, 0.8);
        st.matrix.push_back(MatrixCell{MatrixTab::Generators, 0, EncoderParam::RenderWidth, 0.6});
        setParam(st, EncoderParam::MotionSpeed, 20.0);
        setParam(st, EncoderParam::Lfo1Shape, 3);  // sample & hold: the one with an RNG in it
        setParam(st, EncoderParam::Lfo1Sync, 0);

        ModulationEngine m;
        m.useManifest(encodeMod());
        m.prepare(48000.0);
        m.setState(st);
        m.reset();
        MotionClock clock;
        Transport tp;

        std::vector<double> out;
        for (int i = 0; i < 1500; ++i) {
            const double lvl = 0.5 + 0.5 * std::sin(i * 0.03);
            const std::vector<double> f{lvl, lvl * 0.7, 0.2, 0, 0, 0};
            tp.ppq = i * 0.01;
            m.process(f, {}, tp, kDt);
            clock.advance(m.destination(EncoderParam::MotionSpeed), 6.28, kDt);
            out.push_back(clock.phase());
            out.push_back(m.destination(EncoderParam::RenderWidth));
        }
        return out;
    };
    const auto a = render(), b = render();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);  // bit-identical
}

TEST_CASE("an envelope fires from the source it is routed to, with hysteresis") {
    PluginState st = stateWithCell(MatrixTab::Generators, 3, EncoderParam::RenderWidth, 1.0);
    setParam(st, EncoderParam::RenderWidth, 0.0);
    setParam(st, EncoderParam::Env1Attack, 5.0);  // ms
    setParam(st, EncoderParam::Env1Decay, 5.0);
    setParam(st, EncoderParam::Env1Sustain, 1.0);
    setParam(st, EncoderParam::Env1Release, 5.0);
    st.envTriggers[0].input = TriggerInput::Audio;  // this test is about the audio threshold
    st.envTriggers[0].source = 2;                   // self Tonal, not the default Attack
    st.envTriggers[0].threshold = 0.5;
    st.envTriggers[0].hysteresis = 0.2;

    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;

    auto step = [&](double tonal, int n) {
        const std::vector<double> f{0.0, 1.0, tonal, 0, 0, 0};  // attack high, tonal varies
        for (int i = 0; i < n; ++i) m.process(f, {}, tp, kDt);
    };

    step(0.0, 200);
    INFO("below threshold, width ", m.destination(EncoderParam::RenderWidth));
    CHECK(m.destination(EncoderParam::RenderWidth) < 1.0);  // Attack being high must not fire it

    step(0.8, 200);
    CHECK(m.destination(EncoderParam::RenderWidth) > 150.0);  // routed source crossed: it fires

    //  Between (threshold - hysteresis) and threshold it must stay on rather than chatter.
    step(0.4, 200);
    CHECK(m.destination(EncoderParam::RenderWidth) > 150.0);

    step(0.1, 400);
    CHECK(m.destination(EncoderParam::RenderWidth) < 1.0);  // and release below the lower edge
}

TEST_CASE("parameters change without replacing the state, and the routing survives") {
    PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::RenderWidth, 0.5);
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);

    const std::vector<double> level{1.0, 0, 0, 0, 0, 0};
    Transport tp;
    for (int i = 0; i < 2000; ++i) m.process(level, {}, tp, kDt);
    //  base 0 + level 1 x amount 1 x depth 0.5 x span 180 = 90
    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(90.0).epsilon(0.01));

    auto p = st.params;
    p[static_cast<std::size_t>(EncoderParam::RenderWidth)] = 30.0f;
    p[static_cast<std::size_t>(EncoderParam::ModAmountLevel)] = 0.5f;
    m.setParameters(p);
    for (int i = 0; i < 2000; ++i) m.process(level, {}, tp, kDt);

    //  30 + 1 x 0.5 x 0.5 x 180 = 75: the new base AND the new amount took effect, and the
    //  matrix cell that only setState() installed is still there.
    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(75.0).epsilon(0.01));
}

// ------------------------------------------------------------------- patches adopted by pointer

TEST_CASE("a patch compiled elsewhere and adopted by pointer is the same engine as setState, bit for bit") {
    //  The plugin compiles on the message thread and adopts on the audio thread; bambi-render
    //  and the tests above use setState. They must not be two engines -- including the LFO seeds,
    //  which setState alone sets: two S&H LFOs routed to two destinations diverge at once if
    //  either engine seeds them differently.
    PluginState st = stateWithCell(MatrixTab::Generators, 0, EncoderParam::RenderWidth, 0.7);
    st.matrix.push_back({MatrixTab::Generators, 1, EncoderParam::MotionSpeed, 0.4});
    st.matrix.push_back({MatrixTab::Generators, 3, EncoderParam::TransformYaw, 0.5});
    st.matrix.push_back({MatrixTab::Features, 0, EncoderParam::RenderGain, 0.3});
    for (int i = 0; i < 2; ++i) {
        const int base = static_cast<int>(EncoderParam::Lfo1Rate) + i * 5;
        setParam(st, static_cast<ParamId>(base + 0), 3.0 + i);  // rate
        setParam(st, static_cast<ParamId>(base + 1), 0.0);      // free-running
        setParam(st, static_cast<ParamId>(base + 3), 3.0);      // sample & hold
    }
    st.envTriggers[0].input = TriggerInput::Audio;
    st.envTriggers[0].source = 0;
    st.envTriggers[0].threshold = 0.2;
    st.envTriggers[0].hysteresis = 0.05;

    ModulationEngine viaState, viaPatch;
    viaState.useManifest(encodeMod());
    viaPatch.useManifest(encodeMod());
    viaState.prepare(48000.0);
    viaState.setState(st);
    const ModulationPatch patch = ModulationPatch::compile(encodeMod(), st);
    viaPatch.prepare(48000.0);
    viaPatch.usePatch(patch);
    viaPatch.setParameters(st.params);

    Transport tp;
    tp.playing = true;  // a free LFO set to restart runs only while playing
    long differing = 0;
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 4000; ++i) {
        const double level = 0.5 + 0.5 * std::sin(0.01 * i);
        const std::vector<double> feats{level, 0.3, 0.2, 0, 0, 0};
        viaState.process(feats, {}, tp, kDt);
        viaPatch.process(feats, {}, tp, kDt);
        for (int p = 0; p < kNumEncoderParams; ++p)
            if (viaState.destination(static_cast<ParamId>(p)) != viaPatch.destination(static_cast<ParamId>(p)))
                ++differing;
        lo = std::min(lo, viaState.destination(EncoderParam::MotionSpeed));
        hi = std::max(hi, viaState.destination(EncoderParam::MotionSpeed));
    }
    CHECK(hi - lo > 1.0);  // not vacuous: the S&H LFO really moved its destination
    CHECK(differing == 0);
}

TEST_CASE("adopting a new patch changes the routing without restarting the generators") {
    PluginState a = stateWithCell(MatrixTab::Generators, 0, EncoderParam::RenderWidth, 1.0);
    setParam(a, EncoderParam::RenderWidth, 0.0);
    setParam(a, EncoderParam::Lfo1Sync, 0.0);
    PluginState b = a;
    b.matrix.clear();  // the edit: the cell is removed

    const ModulationPatch pa = ModulationPatch::compile(encodeMod(), a), pb = ModulationPatch::compile(encodeMod(), b);
    ModulationEngine continued, fresh;
    continued.useManifest(encodeMod());
    fresh.useManifest(encodeMod());
    for (auto* e : {&continued, &fresh}) {
        e->prepare(48000.0);
        e->usePatch(pa);
        e->setParameters(a.params);
    }
    Transport tp;
    const std::vector<double> none(6, 0.0);
    for (int i = 0; i < 300; ++i) continued.process(none, {}, tp, kDt);
    continued.usePatch(pb);
    continued.usePatch(pa);  // and back: a round trip through an edit
    for (int i = 0; i < 300; ++i) continued.process(none, {}, tp, kDt);

    for (int i = 0; i < 600; ++i) fresh.process(none, {}, tp, kDt);
    CHECK(continued.destination(EncoderParam::RenderWidth) == fresh.destination(EncoderParam::RenderWidth));
}

// ------------------------------------------------------------------ midi-triggered envelopes

namespace {

/// All three envelopes with short, known stage times; nothing routed, so sources are read directly.
PluginState midiEnvelopes(double attackMs = 10.0, double decayMs = 10.0, double sustain = 0.5,
                          double releaseMs = 10.0) {
    PluginState st{encodeParams()};
    for (int i = 0; i < kNumEnvelopes; ++i) {
        const int base = static_cast<int>(EncoderParam::Env1Attack) + i * 4;
        setParam(st, static_cast<ParamId>(base + 0), attackMs);
        setParam(st, static_cast<ParamId>(base + 1), decayMs);
        setParam(st, static_cast<ParamId>(base + 2), sustain);
        setParam(st, static_cast<ParamId>(base + 3), releaseMs);
    }
    return st;
}

double envelope(const ModulationEngine& m, int index) { return m.sourceValue(15 + index); }

void steps(ModulationEngine& m, int n) {
    Transport tp;
    const std::vector<double> silent(6, 0.0);
    for (int i = 0; i < n; ++i) m.process(silent, {}, tp, kDt);
}

}  // namespace

TEST_CASE("by default each envelope fires from its own drum-map note, and only from that") {
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(midiEnvelopes());

    m.note({10, 38, 1.0f, true});  // D1, the snare
    steps(m, 2);
    CHECK(envelope(m, 0) == 0.0);
    CHECK(envelope(m, 1) > 0.0);
    CHECK(envelope(m, 2) == 0.0);

    m.note({10, 60, 1.0f, true});  // a note nobody listens to
    m.note({10, 36, 1.0f, true});  // C1, the kick
    steps(m, 2);
    CHECK(envelope(m, 0) > 0.0);
    CHECK(envelope(m, 2) == 0.0);
}

TEST_CASE("a note outside the range, or on another channel, does nothing; the range is inclusive") {
    PluginState st = midiEnvelopes();
    st.envTriggers[0].noteLow = 48;
    st.envTriggers[0].noteHigh = 52;
    st.envTriggers[0].channel = 2;
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);

    m.note({1, 50, 1.0f, true});  // right note, wrong channel
    m.note({2, 53, 1.0f, true});  // right channel, one above the range
    steps(m, 2);
    CHECK(envelope(m, 0) == 0.0);

    m.note({2, 48, 1.0f, true});  // the low edge
    steps(m, 2);
    CHECK(envelope(m, 0) > 0.0);
}

TEST_CASE("held: open while any matching note is down, and released only after the last") {
    PluginState st = midiEnvelopes();
    st.envTriggers[0].gate = TriggerGate::Held;
    st.envTriggers[0].noteLow = 36;
    st.envTriggers[0].noteHigh = 40;
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);

    m.note({1, 36, 1.0f, true});
    m.note({1, 38, 1.0f, true});
    m.note({1, 38, 1.0f, true});  // a doubled note-on must not need two note-offs
    steps(m, 20);
    CHECK(envelope(m, 0) == doctest::Approx(0.5));  // at sustain

    m.note({1, 36, 0.0f, false});
    steps(m, 20);
    CHECK(envelope(m, 0) == doctest::Approx(0.5));  // 38 is still down

    m.note({1, 38, 0.0f, false});
    steps(m, 20);
    CHECK(envelope(m, 0) == 0.0);

    //  A held note released inside one control hop plays through instead of parking at sustain.
    m.note({1, 37, 1.0f, true});
    m.note({1, 37, 0.0f, false});
    steps(m, 20);
    CHECK(envelope(m, 0) == 0.0);
}

TEST_CASE("one-shot: attack, decay and release run through, whatever note-off does") {
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(midiEnvelopes());  // one-shot is the default

    m.note({10, 36, 1.0f, true});
    m.note({10, 36, 0.0f, false});  // a drum pad's note is this short
    double peak = 0.0;
    for (int i = 0; i < 3; ++i) {
        steps(m, 1);
        peak = std::max(peak, envelope(m, 0));
    }
    CHECK(peak == doctest::Approx(1.0));
    steps(m, 20);
    CHECK(envelope(m, 0) == 0.0);  // it did not wait at sustain
}

TEST_CASE("a hit while the envelope still sounds rises from where it is, not from zero") {
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    //  A slow attack, so one step of it is small: rising from where the envelope is clears the
    //  earlier value, rising from zero lands far below it. With a fast attack a single step from
    //  zero would already overshoot, and the test could not tell the two apart.
    m.setState(midiEnvelopes(100.0, 10.0, 0.5, 1000.0));  // and a long release

    m.note({10, 36, 1.0f, true});
    steps(m, 30);
    const double before = envelope(m, 0);
    REQUIRE(before > 0.1);
    REQUIRE(before < 0.5);  // releasing

    m.note({10, 36, 1.0f, true});
    steps(m, 1);
    CHECK(envelope(m, 0) > before);
}

TEST_CASE("velocity scales the envelope by the trigger's velocity amount") {
    const auto peakAt = [](double amount, float velocity) {
        PluginState st = midiEnvelopes();
        st.envTriggers[0].velocity = amount;
        ModulationEngine m;
        m.useManifest(encodeMod());
        m.prepare(48000.0);
        m.setState(st);
        m.note({10, 36, velocity, true});
        double peak = 0.0;
        for (int i = 0; i < 4; ++i) {
            steps(m, 1);
            peak = std::max(peak, envelope(m, 0));
        }
        return peak;
    };
    CHECK(peakAt(0.0, 0.5f) == doctest::Approx(1.0));  // amount 0: every hit alike
    CHECK(peakAt(1.0, 0.5f) == doctest::Approx(0.5));
    CHECK(peakAt(0.5, 0.2f) == doctest::Approx(0.6));  // 1 - 0.5 + 0.5 * 0.2
}

TEST_CASE("all notes off closes every held gate") {
    PluginState st = midiEnvelopes();
    st.envTriggers[0].gate = TriggerGate::Held;
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    m.note({10, 36, 1.0f, true});
    steps(m, 20);
    REQUIRE(envelope(m, 0) == doctest::Approx(0.5));
    m.allNotesOff();
    steps(m, 20);
    CHECK(envelope(m, 0) == 0.0);
}

TEST_CASE("an audio-triggered envelope ignores notes, and a MIDI one ignores audio") {
    PluginState st = midiEnvelopes();
    st.envTriggers[0].input = TriggerInput::Audio;  // self Attack over 0.35
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;
    const std::vector<double> loud{1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

    m.note({10, 36, 1.0f, true});  // envelope 0's old note, now audio-triggered
    steps(m, 2);
    CHECK(envelope(m, 0) == 0.0);

    for (int i = 0; i < 2; ++i) m.process(loud, {}, tp, kDt);
    CHECK(envelope(m, 0) > 0.0);   // audio fires it
    CHECK(envelope(m, 1) == 0.0);  // and does not fire a MIDI envelope
}

/*  Spin and roll rate are rates on quantities that wrap, which is what makes an integrator safe
    on them at all. */
TEST_CASE("the rotation clock integrates, wraps, and starts again from nothing") {
    RotationClock r;
    CHECK(r.yawRad() == 0.0);
    CHECK(r.rollRad() == 0.0);

    //  90 deg/s for one second is a quarter turn, whatever the step size it is taken in.
    for (int i = 0; i < 1000; ++i) r.advance(90.0, 0.0, 0.0, 0.001);
    CHECK(r.yawRad() == doctest::Approx(kPi / 2.0).epsilon(1e-9));

    RotationClock coarse;
    for (int i = 0; i < 10; ++i) coarse.advance(90.0, 0.0, 0.0, 0.1);
    CHECK(coarse.yawRad() == doctest::Approx(r.yawRad()).epsilon(1e-9));

    //  Past half a turn it comes round rather than growing without bound.
    RotationClock wrapped;
    for (int i = 0; i < 3000; ++i) wrapped.advance(180.0, 0.0, 0.0, 0.001);  // three half-turns
    CHECK(std::abs(wrapped.yawRad()) <= kPi + 1e-9);
    CHECK(wrapped.yawRad() == doctest::Approx(kPi).epsilon(1e-6));

    //  Roll integrates over time as azimuth does: the same quarter turn in a thousand small steps. Asserted
    //  with dt != 1, where multiplying by dt and forgetting to are the same answer.
    RotationClock slowRoll;
    for (int i = 0; i < 1000; ++i) slowRoll.advance(0.0, 0.0, 90.0, 0.001);
    CHECK(slowRoll.rollRad() == doctest::Approx(kPi / 2.0).epsilon(1e-9));
    CHECK(slowRoll.yawRad() == 0.0);

    //  And pitch: a rotation about the world's left axis, which wraps like the other two.
    RotationClock pitched;
    for (int i = 0; i < 1000; ++i) pitched.advance(0.0, 90.0, 0.0, 0.001);
    CHECK(pitched.pitchRad() == doctest::Approx(kPi / 2.0).epsilon(1e-9));
    CHECK(pitched.yawRad() == 0.0);
    CHECK(pitched.rollRad() == 0.0);

    //  And pitch wraps, which is the property that makes its rate integrable in the first place.
    RotationClock pitchWrap;
    for (int i = 0; i < 3000; ++i) pitchWrap.advance(0.0, 180.0, 0.0, 0.001);  // three half-turns
    CHECK(std::abs(pitchWrap.pitchRad()) <= kPi + 1e-9);
    CHECK(pitchWrap.pitchRad() == doctest::Approx(kPi).epsilon(1e-6));

    //  Negative rates turn the other way, and roll is independent of azimuth.
    RotationClock both;
    both.advance(-45.0, 0.0, 90.0, 1.0);
    CHECK(both.yawRad() == doctest::Approx(-45.0 * kDeg2Rad));
    CHECK(both.rollRad() == doctest::Approx(90.0 * kDeg2Rad));

    both.reset();
    CHECK(both.yawRad() == 0.0);
    CHECK(both.rollRad() == 0.0);
}

TEST_CASE("the three rotation rates are rates; nothing bounded is") {
    CHECK(destinationKind(EncoderParam::TransformYawRate) == DestKind::Rate);
    CHECK(destinationKind(EncoderParam::TransformPitchRate) == DestKind::Rate);
    CHECK(destinationKind(EncoderParam::TransformRollRate) == DestKind::Rate);
    CHECK(destinationKind(EncoderParam::MotionSpeed) == DestKind::Rate);
    CHECK(destinationKind(EncoderParam::TransformYaw) == DestKind::DirectAngle);
    CHECK(destinationKind(EncoderParam::TransformPitch) == DestKind::DirectAngle);
    CHECK(destinationKind(EncoderParam::RenderWidth) == DestKind::DirectScalar);
    CHECK(destinationKind(EncoderParam::TransformExtent) == DestKind::DirectScalar);
}

// ------------------------------------------------------------------- restart or continue

TEST_CASE("continuing, a synced LFO keeps the tempo's rate and runs through a jump in position") {
    const auto apart = [](double a, double b) {
        const double d = std::abs(a - b);
        return std::min(d, 1.0 - d);
    };
    Lfo following, continuing;
    following.prepare(48000.0);
    continuing.prepare(48000.0);
    Transport tp;
    tp.playing = true;
    tp.bpm = 120.0;  // a quarter note is half a second
    for (int i = 0; i < 300; ++i) {
        tp.ppq += tp.bpm / 60.0 * kDt;
        following.process(kDt, tp, true, false, 1.0, 1.0, LfoShape::Saw, 0.0);
        continuing.process(kDt, tp, true, true, 1.0, 1.0, LfoShape::Saw, 0.0);
    }
    CHECK(apart(continuing.phase(), following.phase()) < 1e-9);  // the same rate, while nothing jumps

    tp.ppq = 0.0;  // the loop jumps back
    const double before = continuing.phase();
    following.process(kDt, tp, true, false, 1.0, 1.0, LfoShape::Saw, 0.0);
    continuing.process(kDt, tp, true, true, 1.0, 1.0, LfoShape::Saw, 0.0);
    CHECK(apart(following.phase(), 0.0) < 1e-9);                  // follows the position back
    CHECK(apart(continuing.phase(), before + 2.0 * kDt) < 1e-9);  // runs on at two cycles a second
    CHECK(apart(before, 0.0) > 0.01);                             // and the two are told apart
}

TEST_CASE("a transport restart starts an LFO set to restart again, and leaves one set to continue running") {
    PluginState st{encodeParams()};
    for (int i = 0; i < 2; ++i) {
        const int base = static_cast<int>(EncoderParam::Lfo1Rate) + i * 5;
        setParam(st, static_cast<ParamId>(base + 0), 0.5);  // rate
        setParam(st, static_cast<ParamId>(base + 1), 0.0);  // free-running
    }
    setParam(st, EncoderParam::Lfo1Retrigger, 1.0);  // lfo 1 continues; lfo 2 restarts

    ModulationEngine restarted, untouched, fresh;
    restarted.useManifest(encodeMod());
    untouched.useManifest(encodeMod());
    fresh.useManifest(encodeMod());
    for (auto* m : {&restarted, &untouched, &fresh}) {
        m->prepare(48000.0);
        m->setState(st);
    }
    const std::vector<double> feats(6, 0.0);
    Transport tp;
    tp.playing = true;  // a free LFO set to restart runs only while playing
    for (int i = 0; i < 700; ++i) {
        restarted.process(feats, {}, tp, kDt);
        untouched.process(feats, {}, tp, kDt);
    }
    restarted.restartTransport();
    for (auto* m : {&restarted, &untouched, &fresh}) m->process(feats, {}, tp, kDt);

    CHECK(restarted.sourceValue(12) == doctest::Approx(untouched.sourceValue(12)));  // lfo 1 carried on
    CHECK(restarted.sourceValue(13) == doctest::Approx(fresh.sourceValue(13)));      // lfo 2 started again
    CHECK(std::abs(untouched.sourceValue(13) - fresh.sourceValue(13)) > 0.05);       // which is a real difference

    //  Preparing, or an offline render starting, restarts even an LFO set to continue.
    restarted.reset();
    fresh.reset();
    restarted.process(feats, {}, tp, kDt);
    fresh.process(feats, {}, tp, kDt);
    CHECK(restarted.sourceValue(12) == doctest::Approx(fresh.sourceValue(12)));
}

TEST_CASE("stopped, a free LFO set to restart holds, and one set to continue runs") {
    Lfo restarting, continuing;
    restarting.prepare(48000.0);
    continuing.prepare(48000.0);
    Transport tp;  // stopped
    for (int i = 0; i < 200; ++i) {
        restarting.process(kDt, tp, false, false, 1.0, 1.0, LfoShape::Sine, 0.0);
        continuing.process(kDt, tp, false, true, 1.0, 1.0, LfoShape::Sine, 0.0);
    }
    CHECK(restarting.phase() == 0.0);  // linked to playback: nothing moves until play
    CHECK(continuing.phase() > 0.01);  // running all the time

    tp.playing = true;
    const double held = continuing.phase();
    restarting.process(kDt, tp, false, false, 1.0, 1.0, LfoShape::Sine, 0.0);
    continuing.process(kDt, tp, false, true, 1.0, 1.0, LfoShape::Sine, 0.0);
    CHECK(restarting.phase() == doctest::Approx(kDt));         // playing, it runs from where it held
    CHECK(continuing.phase() == doctest::Approx(held + kDt));  // and the other simply carries on
}

TEST_CASE("clearing one axis's integrated turn leaves the other two where they have turned to") {
    RotationClock r;
    for (int i = 0; i < 100; ++i) r.advance(90.0, 45.0, 30.0, 0.01);
    const double pitch = r.pitchRad(), roll = r.rollRad();
    REQUIRE(std::abs(r.yawRad()) > 0.1);
    r.zero(0);
    CHECK(r.yawRad() == 0.0);
    CHECK(r.pitchRad() == pitch);
    CHECK(r.rollRad() == roll);
    r.zero(2);
    CHECK(r.rollRad() == 0.0);
    CHECK(r.pitchRad() == pitch);
    r.zero(7);  // not an axis
    CHECK(r.pitchRad() == pitch);
}

TEST_CASE("the region is a source like any other: its own value, its own amount, its own tab") {
    PluginState st = stateWithCell(MatrixTab::Region, 0, EncoderParam::RenderWidth, 1.0);
    setParam(st, EncoderParam::RenderWidth, 0.0);
    setParam(st, EncoderParam::ModAmountRegion1, 0.5);
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;

    const std::vector<double> inside{1.0};
    for (int i = 0; i < 2000; ++i) m.process({}, {}, inside, tp, kDt);
    CHECK(m.sourceValue(18) == 0.5);
    CHECK(std::abs(m.destination(EncoderParam::RenderWidth) - 90.0) < 0.5);  // 1 x 0.5 x 1 of 0..180

    /*  Its amount is mod.amount.region1 and nothing else, never found by counting on from the
        first: for slot 18 that lands on lfo1.rate -- 0.25 by default, so the source would read
        0.25 here and ignore the amount below. */
    setParam(st, EncoderParam::ModAmountRegion1, 0.0);
    m.setState(st);
    m.process({}, {}, inside, tp, kDt);
    CHECK(m.sourceValue(18) == 0.0);

    //  handed nothing, or something that is not a value in 0..1, it is still one
    setParam(st, EncoderParam::ModAmountRegion1, 1.0);
    m.setState(st);
    m.process({}, {}, tp, kDt);
    CHECK(m.sourceValue(18) == 0.0);
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), -3.0, 7.0}) {
        const std::vector<double> v{bad};
        m.process({}, {}, v, tp, kDt);
        CHECK(m.sourceValue(18) >= 0.0);
        CHECK(m.sourceValue(18) <= 1.0);
    }
}

TEST_CASE("a column its tab does not have is no source at all") {
    /*  Tabs are six slots apart, so column 6 of the generators would be slot 18 -- the region. A
        hand-edited document could then drive a target from the region while saying "generators". */
    CHECK(sourceSlot(MatrixTab::Generators, 6) == -1);
    CHECK(sourceSlot(MatrixTab::Region, 1) == -1);
    CHECK(sourceSlot(MatrixTab::Features, -1) == -1);
    CHECK(sourceSlot(MatrixTab::Region, 0) == 18);

    PluginState st{encodeParams()};
    st.matrix.push_back(MatrixCell{MatrixTab::Generators, 6, EncoderParam::RenderWidth, 1.0});
    setParam(st, EncoderParam::RenderWidth, 0.0);
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(st);
    Transport tp;
    const std::vector<double> inside{1.0};
    for (int i = 0; i < 500; ++i) m.process({}, {}, inside, tp, kDt);
    CHECK(m.destination(EncoderParam::RenderWidth) == 0.0);
}

TEST_CASE("a parameter that is one turn wraps: past an end it comes back in at the other, the short way round") {
    /*  Catches: clamping instead of wrapping, which sticks displace at 1 for half an LFO's swing;
        smoothing the long way, which sweeps yaw back through front to get from 170 to -170. */
    Transport tp;
    const std::vector<double> off{0.0, 0, 0, 0, 0, 0}, on{1.0, 0, 0, 0, 0, 0};

    //  displace 0.9, pushed 0.3 on: it lands at 0.2, and it is not railing
    {
        PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::MotionDisplace, 0.3);
        setParam(st, EncoderParam::MotionDisplace, 0.9);
        ModulationEngine m;
        m.useManifest(encodeMod());
        m.prepare(48000.0);
        m.setState(st);
        for (int i = 0; i < 400; ++i) m.process(on, {}, tp, kDt);
        CHECK(m.destination(EncoderParam::MotionDisplace) == doctest::Approx(0.2).epsilon(1e-6));
        CHECK_FALSE(m.railing(EncoderParam::MotionDisplace));
    }
    //  yaw 170, pushed 20 degrees on: it crosses 180 to -170 and never passes through front
    {
        PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::TransformYaw, 20.0 / 360.0);
        setParam(st, EncoderParam::TransformYaw, 170.0);
        ModulationEngine m;
        m.useManifest(encodeMod());
        m.prepare(48000.0);
        m.setState(st);
        for (int i = 0; i < 50; ++i) m.process(off, {}, tp, kDt);
        double nearestFront = 180.0;
        for (int i = 0; i < 400; ++i) {
            m.process(on, {}, tp, kDt);
            nearestFront = std::min(nearestFront, std::abs(m.destination(EncoderParam::TransformYaw)));
        }
        CHECK(m.destination(EncoderParam::TransformYaw) == doctest::Approx(-170.0).epsilon(1e-6));
        CHECK(nearestFront > 160.0);
    }
    //  and a parameter with real ends still stops at them, and says so
    {
        PluginState st = stateWithCell(MatrixTab::Features, 0, EncoderParam::RenderWidth, 1.0);
        setParam(st, EncoderParam::RenderWidth, 90.0);
        ModulationEngine m;
        m.useManifest(encodeMod());
        m.prepare(48000.0);
        m.setState(st);
        for (int i = 0; i < 4000; ++i) m.process(on, {}, tp, kDt);
        CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(180.0).epsilon(1e-6));
        CHECK(m.railing(EncoderParam::RenderWidth));
    }
}
