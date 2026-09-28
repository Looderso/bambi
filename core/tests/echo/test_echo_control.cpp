// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <limits>

#include "bambi/dsp/features.hpp"
#include "bambi/echo/control.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/patch/state.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("a fresh instance opens with tap 1 on and three waiting") {
    const EchoSettings s = EchoSettings::defaults();
    CHECK(s.taps[0].on);
    CHECK_FALSE(s.taps[1].on);
    CHECK(s.taps[0].timing.steps == 2);
    CHECK(s.taps[1].timing.offsetSteps == 1);
    CHECK(s.taps[2].axisAzimuthDeg == 90.0);
    CHECK(s.taps[2].axisElevationDeg == 0.0);
    CHECK(s.taps[3].spinDeg == 180.0);  // ping-pong: twice is back where it started
    CHECK(s.taps[3].timing.steps == 3);
    CHECK(s.taps[0].timing.ms == doctest::Approx(250.0));  // what an eighth is at 120 bpm
    CHECK(s.sendShape.kind == RegionKind::Everywhere);
}

TEST_CASE("settings become samples, linear gains and radians, held to their ranges") {
    EchoSettings s = EchoSettings::defaults();
    EchoResolver r;
    EchoFrame f = r.resolve(s, 120.0, 48000.0, 3);
    CHECK(f.taps[0].periodSamples == 12000);  // an eighth at 120 bpm
    CHECK(f.taps[1].offsetSamples == 6000);   // a sixteenth late
    CHECK(f.taps[0].level == doctest::Approx(std::pow(10.0, -3.0 / 20.0)));
    CHECK(f.taps[0].feedback == doctest::Approx(std::pow(10.0, -6.0 / 20.0)));
    CHECK(f.taps[0].pass.spinRad == doctest::Approx(45.0 * kDeg2Rad));
    CHECK(f.taps[0].pass.blurRad == doctest::Approx(6.0 * kDeg2Rad));
    CHECK(f.taps[0].pass.lowCutHz == 80.0);
    CHECK(f.taps[0].pass.highCutHz == 14000.0);
    CHECK(arc(f.taps[2].pass.axis, {0, 1, 0}) < 1e-12);  // azimuth 90: left

    //  past their ranges, and not numbers at all
    s.taps[0].feedbackDb = +6.0;
    s.taps[0].levelDb = 12.0;
    s.taps[0].skew = 3.0;
    s.taps[0].blurDeg = std::numeric_limits<double>::quiet_NaN();
    s.taps[0].highCutHz = 1.0e9;
    f = r.resolve(s, 120.0, 48000.0, 3);
    CHECK(f.taps[0].feedback == doctest::Approx(std::pow(10.0, -0.3 / 20.0)));  // never unity, never above
    CHECK(f.taps[0].level == 1.0);
    CHECK(f.taps[0].pass.skew == 0.6);
    CHECK(f.taps[0].pass.blurRad == doctest::Approx(6.0 * kDeg2Rad));
    CHECK(f.taps[0].pass.highCutHz == 20000.0);
}

TEST_CASE("an axis is the pole when it was SET straight up, and only then") {
    EchoSettings s = EchoSettings::defaults();
    EchoResolver r;
    CHECK(r.resolve(s, 120.0, 48000.0, 3).taps[0].pass.axisIsPole);
    s.taps[0].axisElevationDeg = 89.999;
    CHECK_FALSE(r.resolve(s, 120.0, 48000.0, 3).taps[0].pass.axisIsPole);
    s.taps[0].axisElevationDeg = -90.0;  // straight down is an axis like any other
    CHECK_FALSE(r.resolve(s, 120.0, 48000.0, 3).taps[0].pass.axisIsPole);
}

TEST_CASE("a period is held against a tempo that wobbles in its last bit, and let go when the tempo moves") {
    //  A sixteenth at 120 bpm and 44.1 kHz is 5512.5 samples: exactly between two. Rounded afresh each
    //  step, a tempo one bit either side of 120 would move the period every step, and every move is a
    //  crossfade. Catches: the hold removed.
    EchoSettings s = EchoSettings::defaults();
    s.taps[0].timing.steps = 1;
    EchoResolver r;
    const int first = r.resolve(s, 120.0, 44100.0, 3).taps[0].periodSamples;
    const double up = std::nextafter(120.0, 200.0), down = std::nextafter(120.0, 0.0);
    for (int i = 0; i < 50; ++i) {
        CHECK(r.resolve(s, (i % 2) != 0 ? up : down, 44100.0, 3).taps[0].periodSamples == first);
    }
    //  and the first period does not depend on that bit either, or a bounce would differ from playback
    for (const double tempo : {down, 120.0, up}) {
        EchoResolver fresh;
        CHECK(fresh.resolve(s, tempo, 44100.0, 3).taps[0].periodSamples == 5512);
    }
    //  a real change of tempo is followed at once
    CHECK(r.resolve(s, 121.0, 44100.0, 3).taps[0].periodSamples ==
          static_cast<int>(std::lround(15.0 / 121.0 * 44100.0)));
    //  and after a reset the period is simply what it rounds to
    r.reset();
    CHECK(r.resolve(s, 121.0, 44100.0, 3).taps[0].periodSamples ==
          static_cast<int>(std::lround(15.0 / 121.0 * 44100.0)));
}

TEST_CASE("the order's cap reaches the frame, and the send is the region its settings say") {
    EchoSettings s = EchoSettings::defaults();
    s.taps[0].timing.steps = 16;  // a bar: 2 s at 120
    EchoResolver r;
    CHECK(r.resolve(s, 120.0, 48000.0, 3).taps[0].periodSamples == 96000);
    r.reset();
    CHECK(r.resolve(s, 120.0, 48000.0, 7).taps[0].periodSamples == 48000);  // halved onto the grid

    s.sendShape.kind = RegionKind::Spot;
    s.sendSide = RegionSide::Outside;
    s.sendAmount = 0.5;
    const EchoFrame f = r.resolve(s, 120.0, 48000.0, 3);
    CHECK(f.send.kind == RegionKind::Spot);
    CHECK(f.send.side == RegionSide::Outside);
    CHECK(f.send.size == doctest::Approx(s.send.size * kDeg2Rad));
    CHECK(f.sendAmount == 0.5);
}

/*  Catches: the hold dropped (the turn advancing whatever the transport does), `continue` not
 *  running while stopped, and a restart that clears a turn the user asked to keep -- or keeps one a
 *  render needs cleared. */
TEST_CASE("a region's turn is held, restarted or left running as rates.retrigger says") {
    EchoSettings s = EchoSettings::defaults();
    s.sendShape.kind = RegionKind::Spot;
    s.sendRates.yaw = 90.0;

    EchoResolver r;
    for (int i = 0; i < 100; ++i) r.resolve(s, 120.0, 48000.0, 3, false);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(0.0));  // restart: stopped, it holds

    for (int i = 0; i < 100; ++i) r.resolve(s, 120.0, 48000.0, 3, true);
    const double played = r.sendTurn().yawRad();
    CHECK(played > 0.1);  // and turns while it plays
    r.restartTransport(s.ratesContinue);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(0.0));  // a play start takes it back

    s.ratesContinue = true;
    for (int i = 0; i < 100; ++i) r.resolve(s, 120.0, 48000.0, 3, false);
    const double ran = r.sendTurn().yawRad();
    CHECK(ran == doctest::Approx(played));  // continue: it runs, stopped or not
    r.restartTransport(s.ratesContinue);
    CHECK(r.sendTurn().yawRad() == doctest::Approx(ran));  // and a play start leaves it
}

TEST_CASE("an LFO on a region's fill moves the send's shape, step by step") {
    /*  The seven shape settings are modulation targets, and a control step hands the engine the
     *  modulated shape. Catches: fill left out of the shared target table, so the send holds still. */
    const ParamManifest& m = echoParams();
    PluginState st{m};
    const auto fill = static_cast<ParamId>(m.byKey("region1.fill"));
    st.matrix.push_back({MatrixTab::Generators, 0, fill, 0.5});        // LFO 1, half depth
    st.params[static_cast<std::size_t>(m.byKey("lfo1.sync"))] = 0.0f;  // free, at 2 Hz: the steps move no song position
    st.params[static_cast<std::size_t>(m.byKey("lfo1.rate"))] = 2.0f;
    ModulationEngine mod;
    mod.useManifest(echoMod());
    mod.prepare(48000.0);
    mod.setState(st);
    EchoResolver r;
    std::array<double, kNumFeatures> self{};
    EchoControlInput in;
    in.self = self;
    in.dt = static_cast<double>(kEchoHop) / 48000.0;
    in.order = 3;
    in.transport.bpm = 120.0;
    in.transport.playing = true;
    in.sendShape = {RegionKind::Sectors, 4, 6};
    const std::array<float, kMaxParams> base = st.params;
    double lo = 1.0, hi = 0.0;
    for (int i = 0; i < 400; ++i) {
        const double f = r.step(mod, m, base, in, 48000.0).send.fill;
        lo = std::min(lo, f), hi = std::max(hi, f);
    }
    INFO("the send's fill ran from ", lo, " to ", hi);
    CHECK(hi - lo > 0.2);
}
