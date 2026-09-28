// SPDX-License-Identifier: GPL-3.0-or-later
//
//  A modulation source's settings: what the editor draws of LFOs, envelopes and features, and the
//  trigger edits it makes.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "bambi/encode/params.hpp"
#include "bambi/mod/sources.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("the slots are six features of each input, three LFOs, three envelopes and the region") {
    CHECK(sourceAt(0).kind == SourceKind::Feature);
    CHECK(sourceAt(0).index == static_cast<int>(Feature::Level));
    CHECK_FALSE(sourceAt(0).sidechain);
    CHECK(sourceAt(7).kind == SourceKind::Feature);
    CHECK(sourceAt(7).index == static_cast<int>(Feature::Attack));
    CHECK(sourceAt(7).sidechain);
    CHECK(sourceAt(12).kind == SourceKind::Lfo);
    CHECK(sourceAt(12).index == 0);
    CHECK(sourceAt(14).index == 2);
    CHECK(sourceAt(15).kind == SourceKind::Envelope);
    CHECK(sourceAt(15).index == 0);
    CHECK(sourceAt(17).index == 2);
    CHECK(sourceAt(18).kind == SourceKind::Region);
    CHECK(sourceAt(18).index == 0);
    CHECK(sourceAt(19).kind == SourceKind::None);
    CHECK(sourceAt(-1).kind == SourceKind::None);
    CHECK(sourceAt(sourceSlot(MatrixTab::Generators, 4)).kind == SourceKind::Envelope);
    CHECK(sourceAt(sourceSlot(MatrixTab::Generators, 4)).index == 1);
}

TEST_CASE("an LFO's drawn shape is the shape the engine plays") {
    for (const auto shape : {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw}) {
        Lfo lfo;
        lfo.prepare(48000.0);
        const double dt = 256.0 / 48000.0;
        const double offset = 0.3;
        double worst = 0.0;
        for (int i = 0; i < 400; ++i) {
            const double v = lfo.process(dt, Transport{true, 120.0, 0.0}, false, false, 1.7, 1.0, shape, offset);
            const double phase = lfo.phase() + offset;
            worst = std::max(worst, std::abs(v - lfoShapeValue(shape, phase - std::floor(phase))));
        }
        CHECK(worst < 1e-12);
    }
    CHECK(lfoShapeValue(LfoShape::Sine, 0.25) == doctest::Approx(1.0));
    CHECK(lfoShapeValue(LfoShape::Triangle, 0.0) == doctest::Approx(1.0));
    CHECK(lfoShapeValue(LfoShape::Triangle, 0.5) == doctest::Approx(-1.0));
    CHECK(lfoShapeValue(LfoShape::Saw, 0.75) == doctest::Approx(0.5));
}

TEST_CASE("an LFO outline spans the width from its phase offset, and sample-and-hold draws as steps") {
    const auto sine = lfoOutline(LfoShape::Sine, 90.0, 2, 32);
    REQUIRE(sine.size() == 65);
    CHECK(sine.front().x == 0.0);
    CHECK(sine.back().x == doctest::Approx(1.0));
    CHECK(sine.front().y == doctest::Approx(1.0));  // a quarter turn in: the peak
    CHECK(sine[16].y == doctest::Approx(-1.0));     // half a cycle on: the trough

    const auto sh = lfoOutline(LfoShape::SampleHold, 90.0, 6, 32);
    REQUIRE(sh.size() == 12);
    CHECK(sh[0].y == sh[1].y);  // level within a cycle
    CHECK(sh[1].x == sh[2].x);  // a vertical edge at the latch
    CHECK(sh[1].y != sh[2].y);
    CHECK(sh.back().x == doctest::Approx(1.0));
    CHECK(std::all_of(sh.begin(), sh.end(), [](const OutlinePoint& p) { return std::abs(p.y) <= 1.0; }));
}

TEST_CASE("an envelope outline keeps every stage visible and in order, at the sustain the engine holds") {
    const auto held = envelopeOutline(10.0, 200.0, 0.5, 400.0, TriggerGate::Held);
    CHECK(held[0].x == 0.0);
    CHECK(held[0].y == 0.0);
    CHECK(held[1].y == 1.0);
    CHECK(held[2].y == 0.5);
    CHECK(held[3].y == 0.5);
    CHECK(held[4].x == doctest::Approx(1.0));
    CHECK(held[4].y == 0.0);
    for (std::size_t i = 1; i < held.size(); ++i) CHECK(held[i].x > held[i - 1].x);

    //  Linear time would give a 10 ms attack 1/91 of the width here: three pixels of the panel.
    CHECK(held[1].x > 0.04);
    CHECK(envelopeOutline(100.0, 200.0, 0.5, 400.0, TriggerGate::Held)[1].x > held[1].x);

    const auto shot = envelopeOutline(10.0, 200.0, 0.5, 400.0, TriggerGate::OneShot);
    CHECK(shot[3].x == shot[2].x);  // a one-shot never waits at sustain

    CHECK(envelopeOutline(10.0, 200.0, 1.7, 400.0, TriggerGate::Held)[2].y == 1.0);
}

TEST_CASE("notes are named as the General MIDI drum defaults are: C1 is 36, middle C is C3") {
    CHECK(noteName(36) == "C1");
    CHECK(noteName(38) == "D1");
    CHECK(noteName(42) == "F#1");
    CHECK(noteName(60) == "C3");
    CHECK(noteName(127) == "G8");
    CHECK(noteName(0) ==
          "C\xe2\x88\x92"
          "2");
    CHECK(noteRangeName(36, 36) == "C1");
    CHECK(noteRangeName(36, 40) == "C1 \xe2\x80\x93 E1");
    CHECK(channelName(0) == "any");
    CHECK(channelName(10) == "10");

    const std::array<std::string, 3> defaults{"C1", "D1", "F#1"};
    for (int i = 0; i < 3; ++i)
        CHECK(noteName(EnvTrigger::defaults(i).noteLow) == defaults[static_cast<std::size_t>(i)]);
}

TEST_CASE("a trigger's note range never inverts, and its other settings stay in range") {
    EnvTrigger t = EnvTrigger::defaults(0);
    setTriggerHigh(t, 40);
    CHECK(t.noteLow == 36);
    CHECK(t.noteHigh == 40);
    setTriggerLow(t, 45);  // past the top: carries it along
    CHECK(t.noteLow == 45);
    CHECK(t.noteHigh == 45);
    setTriggerHigh(t, 30);  // below the bottom: carries it along
    CHECK(t.noteLow == 30);
    CHECK(t.noteHigh == 30);
    setTriggerLow(t, -5);
    CHECK(t.noteLow == 0);
    setTriggerHigh(t, 300);
    CHECK(t.noteHigh == 127);

    setTriggerChannel(t, 17);
    CHECK(t.channel == 16);
    setTriggerChannel(t, -1);
    CHECK(t.channel == 0);
    setTriggerVelocity(t, 1.5);
    CHECK(t.velocity == 1.0);
    setTriggerSource(t, 15);  // an envelope cannot fire from a generator
    CHECK(t.source == kTriggerSources - 1);

    t.input = TriggerInput::Audio;
    learnTriggerNote(t, 10, 40);
    CHECK(t.input == TriggerInput::Midi);
    CHECK(t.noteLow == 40);
    CHECK(t.noteHigh == 40);
    CHECK(t.channel == 10);
}

TEST_CASE("hysteresis never exceeds the threshold, or a fired envelope would never let go") {
    PluginState st{encodeParams()};
    auto& t = st.envTriggers[0];
    t.input = TriggerInput::Audio;
    t.source = static_cast<int>(Feature::Level);
    setTriggerThreshold(t, 0.2);
    setTriggerHysteresis(t, 0.5);
    CHECK(t.hysteresis == doctest::Approx(0.2));
    setTriggerThreshold(t, 0.1);  // lowering the threshold takes the hysteresis down with it
    CHECK(t.hysteresis == doctest::Approx(0.1));

    //  In the engine: loud, then silence far longer than the release. The envelope is back at zero.
    ModulationEngine engine;
    engine.useManifest(encodeMod());
    engine.prepare(48000.0);
    engine.setState(st);
    engine.reset();
    const double dt = 256.0 / 48000.0;
    const std::array<double, kNumFeatures> loud{1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const std::array<double, kNumFeatures> quiet{};
    for (int i = 0; i < 40; ++i) engine.process(loud, {}, Transport{}, dt);
    const bool fired = engine.sourceValue(15) > 0.0;
    for (int i = 0; i < 1000; ++i) engine.process(quiet, {}, Transport{}, dt);
    CHECK(fired);
    CHECK(engine.sourceValue(15) == 0.0);
}

TEST_CASE("a feature's calibration is read from the detector's configuration, so the text cannot drift") {
    const auto has = [](const std::vector<SourceFact>& facts, const std::string& label, const std::string& value) {
        return std::any_of(facts.begin(), facts.end(),
                           [&](const SourceFact& f) { return f.label == label && f.value == value; });
    };
    const std::string minus = "\xe2\x88\x92";
    CHECK(has(featureCalibration(Feature::Level), "range", minus + "55 to " + minus + "6 dB"));
    CHECK(has(featureCalibration(Feature::Attack), "ratio", "1.0 to 2.2"));
    CHECK(has(featureCalibration(Feature::Tonal), "band", "100 hz to 8 khz"));
    CHECK(has(featureCalibration(Feature::Mid), "band", "250 hz to 2 khz"));

    FeatureConfig cfg;
    cfg.levelHiDb = -3.0;
    cfg.bandSplitHiHz = 2500.0;
    CHECK(has(featureCalibration(Feature::Level, cfg), "range", minus + "55 to " + minus + "3 dB"));
    CHECK(has(featureCalibration(Feature::High, cfg), "band", "above 2.5 khz"));

    for (int f = 0; f < kNumFeatures; ++f) {
        //  every feature is gated but Level, whose own range ends where the gate opens
        CHECK(has(featureCalibration(static_cast<Feature>(f)), "gate", minus + "55 dB") ==
              (static_cast<Feature>(f) != Feature::Level));
    }
}

/*  The drawn shape has to be the shape that plays. A picture drawn from straight lines beside a
 *  curved stage is a picture that lies, which is the whole reason the curve exists at all.
 *
 *  Catches: the curve ignored in the drawing (every sample lands on the straight line), and the
 *  curve applied to x as well as y (the stage would no longer end where its corner says). */
TEST_CASE("the drawn envelope is bowed by the same curve the engine plays") {
    const auto straight = envelopeCurve(50.0, 200.0, 0.5, 400.0, TriggerGate::Held, 0.0, 0.0, 0.0, 16);
    const auto bowed = envelopeCurve(50.0, 200.0, 0.5, 400.0, TriggerGate::Held, 0.6, 0.0, 0.0, 16);
    REQUIRE(straight.size() == bowed.size());

    //  the corners are where they were: a curve bows the stage, it does not move its ends
    CHECK(bowed.front().x == doctest::Approx(straight.front().x));
    CHECK(bowed.front().y == doctest::Approx(straight.front().y));
    CHECK(bowed.back().x == doctest::Approx(straight.back().x));
    CHECK(bowed.back().y == doctest::Approx(straight.back().y));

    //  and in between, a positive attack curve is above the straight line at the same x
    bool anyAbove = false, everyXSame = true;
    for (std::size_t i = 0; i < straight.size(); ++i) {
        if (std::abs(bowed[i].x - straight[i].x) > 1e-12) everyXSame = false;
        if (bowed[i].y > straight[i].y + 1e-6) anyAbove = true;
    }
    CHECK(anyAbove);
    CHECK(everyXSame);

    //  a linear curve draws exactly the straight line the five corners describe
    const auto corners = envelopeOutline(50.0, 200.0, 0.5, 400.0, TriggerGate::Held);
    CHECK(straight[16].x == doctest::Approx(corners[1].x));
    CHECK(straight[16].y == doctest::Approx(corners[1].y));
}

TEST_CASE("a stage's x is its own time and nothing else's, so a drag inverts exactly") {
    /*  Catches: a normalised x that divides by the live total, so dragging any stage moves every
        other point and the dragged one drifts from the cursor. */
    EnvelopeShape env{};
    const auto before = envelopeStops(env);
    env.releaseMs = 4000.0;
    const auto after = envelopeStops(env);
    CHECK(after[1] == doctest::Approx(before[1]));  // attack did not move
    CHECK(after[2] == doctest::Approx(before[2]));  // nor the end of decay
    CHECK(after[3] == doctest::Approx(before[3]));  // nor the hold's right edge
    CHECK(after[4] > before[4]);                    // only the stage that changed

    //  and the inversion: a target x solves back to the ms that puts the point exactly there
    const double x = envelopeStops(env)[1] + 18.0;
    const double ms = envelopeStageMs(env, EnvelopeGrab::DecaySustain, x, 0.1, 5000.0);
    EnvelopeShape moved = env;
    moved.decayMs = ms;
    CHECK(envelopeStops(moved)[2] == doctest::Approx(x));

    //  a stage is held inside the manifest's own bounds rather than following the cursor out
    CHECK(envelopeStageMs(env, EnvelopeGrab::Attack, 1e6, 0.1, 2000.0) == doctest::Approx(2000.0));
    CHECK(envelopeStageMs(env, EnvelopeGrab::Attack, -5.0, 0.1, 2000.0) == doctest::Approx(0.1));
    //  the hold's edge is a preview width, not a parameter: it solves for no time at all
    CHECK(envelopeStageMs(env, EnvelopeGrab::SustainEdge, 20.0, 0.1, 5000.0) == doctest::Approx(0.0));
}

TEST_CASE("the plot keeps one scale for ordinary values and only grows for an extreme one") {
    EnvelopeShape env{};
    const double span = envelopeSpan(env);
    env.attackMs = 300.0;
    CHECK(envelopeSpan(env) == doctest::Approx(span));  // still inside the reference, unmoved
    CHECK(envelopeStops(env)[4] < span);                // and with room left to drag into
    env.releaseMs = 8000.0;
    CHECK(envelopeSpan(env) > span);  // past it, the plot gives the room
    CHECK(envelopeSpan(env) == doctest::Approx(envelopeStops(env)[4]));
}

TEST_CASE("a point is grabbed at its end, a curve at its body, and nothing from empty space") {
    const EnvelopeShape env{};
    const auto stops = envelopeStops(env);
    const double xSlop = 1.0, ySlop = 0.06;

    CHECK(envelopeGrabAt(env, stops[1], 1.0, xSlop, ySlop) == EnvelopeGrab::Attack);
    CHECK(envelopeGrabAt(env, stops[2], env.sustain, xSlop, ySlop) == EnvelopeGrab::DecaySustain);
    CHECK(envelopeGrabAt(env, stops[4], 0.0, xSlop, ySlop) == EnvelopeGrab::Release);
    //  dead centre on a stage's body is the CURVE's, and the handle drawn there is on the line
    const auto mid = envelopeGrabPoint(env, EnvelopeGrab::DecayCurve);
    CHECK(envelopeGrabAt(env, mid.x, mid.y, xSlop, ySlop) == EnvelopeGrab::DecayCurve);
    CHECK(mid.x == doctest::Approx(0.5 * (stops[1] + stops[2])));
    //  near a stage's end belongs to the end, not to the body running into it
    CHECK(envelopeGrabAt(env, stops[2] - 0.2 * xSlop, env.sustain, xSlop, ySlop) == EnvelopeGrab::DecaySustain);
    /*  A click on the line but within a corner's reach in x is that corner's business, not the
        body's: the attack is steep enough that a point a whisker left of the peak sits exactly on
        the curve while being nowhere near the peak in value. */
    const double nearPeak = stops[1] - 0.5 * xSlop;
    const double onLine = ease(nearPeak / stops[1], env.attackCurve);
    CHECK(envelopeGrabAt(env, nearPeak, onLine, xSlop, ySlop) == EnvelopeGrab::None);
    CHECK(envelopeGrabAt(env, 0.5 * stops[1], ease(0.5, env.attackCurve), xSlop, ySlop) ==
          EnvelopeGrab::AttackCurve);  // and the middle of the same stage still is the body's
    //  empty space grabs nothing at all
    CHECK(envelopeGrabAt(env, mid.x, mid.y + 10.0 * ySlop, xSlop, ySlop) == EnvelopeGrab::None);
    /*  The plateau is drawn whatever the gate: it previews however long a note is held, which
        the envelope does not own. Omitting it on a one-shot showed no sustain at all. */
    CHECK(envelopeStops(env)[3] > envelopeStops(env)[2]);
    CHECK(envelopeGrabAt(env, stops[3], env.sustain, xSlop, ySlop) == EnvelopeGrab::SustainEdge);
}

TEST_CASE("an upward drag raises a stage, whether it rises or falls") {
    //  Catches: one sign for all three, which bends two of them away from the mouse
    const double up = -10.0;  // dy is down the screen
    const EnvelopeShape env{};
    const auto higher = [&env](EnvelopeGrab grab, double curve) {
        EnvelopeShape bowed = env;
        (grab == EnvelopeGrab::AttackCurve  ? bowed.attackCurve
         : grab == EnvelopeGrab::DecayCurve ? bowed.decayCurve
                                            : bowed.releaseCurve) = curve;
        return envelopeGrabPoint(bowed, grab).y;
    };
    for (const auto grab : {EnvelopeGrab::AttackCurve, EnvelopeGrab::DecayCurve, EnvelopeGrab::ReleaseCurve})
        CHECK(higher(grab, envelopeBow(0.0, grab, up, 90.0)) > higher(grab, 0.0));

    CHECK(envelopeBow(0.9, EnvelopeGrab::AttackCurve, -1000.0, 90.0) == doctest::Approx(1.0));
    CHECK(envelopeBow(-0.9, EnvelopeGrab::AttackCurve, 1000.0, 90.0) == doctest::Approx(-1.0));
    //  a point is not a curve: dragging one bows nothing
    CHECK(envelopeBow(0.3, EnvelopeGrab::Attack, up, 90.0) == doctest::Approx(0.3));
}

TEST_CASE("the axis is nice values placed by polarity, not fixed rows relabelled") {
    const auto unipolar = polarityAxis(0.0, 5);
    REQUIRE(!unipolar.empty());
    CHECK(unipolar.front().at == doctest::Approx(0.0));
    CHECK(unipolar.back().at == doctest::Approx(1.0));
    CHECK(unipolar.front().zero);
    for (const auto& tick : unipolar) {
        CHECK(tick.at >= 0.0);
        CHECK(tick.at <= 1.0);
        //  a nice value: a multiple of the step, never whatever the map happened to land on
        CHECK(std::abs(tick.value * 100.0 - std::round(tick.value * 100.0)) < 1e-6);
    }

    /*  The tell that the grid is solved and not relabelled: zero's pixel row moves with polarity,
        and so does every other tick's -- the ceiling stays at 1 either way. */
    const auto bipolar = polarityAxis(1.0, 5);
    const auto zeroAt = [](const std::vector<AxisTick>& ticks) {
        for (const auto& tick : ticks)
            if (tick.zero) return tick.at;
        return -1.0;
    };
    CHECK(zeroAt(unipolar) == doctest::Approx(0.0));
    CHECK(zeroAt(bipolar) == doctest::Approx(0.5));
    CHECK(bipolar.back().value == doctest::Approx(1.0));
    CHECK(bipolar.front().value == doctest::Approx(-1.0));

    /*  The tell that a row is solved and not a fixed fraction relabelled: at a polarity whose floor
        is not a multiple of the step, zero lands where the map puts it and nowhere else. */
    const auto odd = polarityAxis(0.7, 5);
    for (const auto& tick : odd)
        if (tick.zero) CHECK(tick.at == doctest::Approx(0.7 / 1.7).epsilon(0.001));
    CHECK(odd.back().value == doctest::Approx(1.0));
    CHECK(odd.back().at == doctest::Approx(1.0));
    CHECK(odd.front().value >= -0.7);  // a nice value below the floor is not a line on this plot

    CHECK(axisTickLabel(0.5) == "0.5");
    CHECK(axisTickLabel(-1.0) ==
          "\u2212"
          "1");  // the typographic minus every number wears
    CHECK(axisTickLabel(0.0) == "0");
}
