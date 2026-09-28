// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <vector>

#include "bambi/encode/control.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/generator.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kDt = 256.0 / 48000.0;

/*  An orbit, an engine with nothing in its matrix, and the parameters as a plugin holds them. With
 *  no cells a destination is its parameter, smoothed, so each test sets what it needs and lets the
 *  smoothers settle before it measures. */
struct Rig {
    PluginState state{encodeParams()};
    Trajectory path;
    Vec3 centre;
    ModulationEngine mod;
    EncoderControl control;

    Rig() {
        mod.useManifest(encodeMod());
        state.trajectory.generator = GeneratorType::Orbit;
        generatorDefaults(GeneratorType::Orbit, state.trajectory.genParams);
        path.build(state.trajectory);
        centre = pathCentre(path.points());
    }
    void set(ParamId id, float v) { state.params[static_cast<std::size_t>(id)] = v; }
    void start() {
        mod.prepare(48000.0);
        mod.setState(state);
        mod.reset();
        control.reset();
    }
    bool useRegion{false};
    bool stereoInput{true};
    ControlFrame step(bool playing = true, int zeroTurns = 0, int zeroRegionTurns = 0) {
        ControlInput in;
        if (useRegion) in.region = &state.regions[0];
        in.transport.playing = playing;
        in.dt = kDt;
        in.path = &path;
        in.centre = centre;
        in.shape = &state.trajectory;
        in.zeroTurns = zeroTurns;
        in.zeroRegionTurns = zeroRegionTurns;
        in.stereoInput = stereoInput;
        return control.step(mod, state.params, in);
    }
    ControlFrame run(int steps, bool playing = true) {
        ControlFrame f;
        for (int i = 0; i < steps; ++i) f = step(playing);
        return f;
    }
};

}  // namespace

TEST_CASE("speed moves the source along its path, and direction turns it round") {
    Rig fwd, rev;
    fwd.set(EncoderParam::MotionSpeed, 90.0f);
    rev.set(EncoderParam::MotionSpeed, 90.0f);
    rev.set(EncoderParam::MotionDirection, 1.0f);
    fwd.start();
    rev.start();
    const ControlFrame a = fwd.run(200), b = rev.run(200);
    CHECK(a.speed > 0.0);
    CHECK(b.speed == -a.speed);
    CHECK(fwd.control.motion().phase() > 0.01);
    //  the same distance, the other way: catches a direction that is read and then ignored
    CHECK(rev.control.motion().phase() == doctest::Approx(-fwd.control.motion().phase()));
    CHECK(arc(a.position, b.position) > 0.1);
}

TEST_CASE("stopped, a motion set to restart holds; set to continue, it runs") {
    Rig held, free;
    for (Rig* r : {&held, &free}) {
        r->set(EncoderParam::MotionSpeed, 90.0f);
        r->set(EncoderParam::TransformYawRate, 45.0f);
    }
    free.set(EncoderParam::RatesRetrigger, 1.0f);
    held.start();
    free.start();
    held.run(200, false);
    free.run(200, false);
    CHECK(held.control.motion().phase() == 0.0);
    CHECK(held.control.rotation().yawRad() == 0.0);
    CHECK(free.control.motion().phase() > 0.01);
    CHECK(free.control.rotation().yawRad() > 0.01);
}

TEST_CASE("what a rate has turned is in the placement, on its own axis, and a hand can zero it") {
    Rig r;
    r.set(EncoderParam::TransformYaw, 10.0f);
    r.set(EncoderParam::TransformPitchRate, 30.0f);
    r.start();
    const ControlFrame f = r.run(400);
    const double turned = r.control.rotation().pitchRad();
    CHECK(turned > 0.5);
    //  the set angle plus the turn, each where it belongs: catches the turn left out, and catches
    //  it landing on its neighbour
    CHECK(f.placement.pitchRad == doctest::Approx(turned));
    CHECK(f.placement.yawRad == doctest::Approx(10.0 * kDeg2Rad));
    CHECK(f.placement.rollRad == doctest::Approx(0.0));

    const ControlFrame z = r.step(true, 2);  // pitch set back to zero by hand
    CHECK(z.placement.pitchRad < 0.01);      // one step's turn, not four hundred
    CHECK(r.control.rotation().yawRad() == 0.0);
}

TEST_CASE("the first step snaps, the next does not, and a restart snaps again") {
    Rig r;
    r.start();
    CHECK(r.step().snap);
    CHECK_FALSE(r.step().snap);  // catches a flag that is never cleared
    r.control.restartTransport(false);
    CHECK(r.step().snap);
    CHECK_FALSE(r.step().snap);
    r.control.requestSnap();
    CHECK(r.step().snap);
}

TEST_CASE("a restart takes the motion back to zero, unless the motion continues") {
    Rig r;
    r.set(EncoderParam::MotionSpeed, 90.0f);
    r.set(EncoderParam::TransformRollRate, 45.0f);
    r.start();
    r.run(200);
    const double phase = r.control.motion().phase(), roll = r.control.rotation().rollRad();
    REQUIRE(phase > 0.01);
    REQUIRE(roll > 0.01);

    r.control.restartTransport(true);  // continue: carries on from where it was
    CHECK(r.control.motion().phase() == phase);
    CHECK(r.control.rotation().rollRad() == roll);

    r.control.restartTransport(false);
    CHECK(r.control.motion().phase() == 0.0);
    CHECK(r.control.rotation().rollRad() == 0.0);

    //  requestSnap is a state loaded under a playing source: nothing moves
    r.run(50);
    const double later = r.control.motion().phase();
    r.control.requestSnap();
    CHECK(r.control.motion().phase() == later);
}

TEST_CASE("a frame carries width, gain and trim, and the encoder is told to jump or to ramp") {
    Rig r;
    r.set(EncoderParam::RenderWidth, 60.0f);
    r.set(EncoderParam::RenderGain, -6.0f);
    r.set(EncoderParam::InputTrim, 3.0f);
    r.start();
    const ControlFrame f = r.run(400);
    CHECK(f.widthRad == doctest::Approx(60.0 * kDeg2Rad));
    CHECK(f.gainDb == doctest::Approx(-6.0));
    CHECK(f.trimDb == doctest::Approx(3.0));

    //  snapped, the encoder is there at once; ramped, it is not there yet
    Encoder jumped, ramped;
    jumped.prepare(1);
    ramped.prepare(1);
    ControlFrame to;
    to.position = {0.0, 1.0, 0.0};
    to.snap = true;
    applyFrame(jumped, to);
    to.snap = false;
    applyFrame(ramped, to);
    Encoder there;
    there.prepare(1);
    there.setTarget({0.0, 1.0, 0.0}, 0.0);
    there.snap();
    CHECK(jumped.gains()[1] == there.gains()[1]);
    CHECK(ramped.gains()[1] != there.gains()[1]);
}

namespace {

/*  A hard-edged half: everything in front of the listener. An orbit passes in and out of it twice a
 *  lap, and with no softness the value is 0 or 1, so which step it changes on can be read exactly. */
void frontHalf(Rig& r) {
    r.useRegion = true;
    r.state.regions[0].shape.kind = RegionKind::Spot;
    r.set(EncoderParam::Region1Size, 90.0f);
    r.set(EncoderParam::Region1Softness, 0.0f);
    r.set(EncoderParam::MotionSpeed, 180.0f);
}

Region asSet(const Rig& r) {
    RegionSettingsDeg set;
    set.size = 90.0;
    set.softness = 0.0;
    const RegionSide side = r.state.params[static_cast<std::size_t>(EncoderParam::Region1Side)] > 0.5f
                                ? RegionSide::Outside
                                : RegionSide::Inside;
    return resolveRegion(r.state.regions[0].shape, side, set);
}

}  // namespace

TEST_CASE("the region source reads where the source was one step ago -- not now, not two ago") {
    Rig r;
    frontHalf(r);
    r.start();
    const Region region = asSet(r);
    ControlFrame before = r.step();
    int changes = 0;
    for (int i = 0; i < 1500; ++i) {
        const ControlFrame now = r.step();
        //  what it was given is the region at the position the step before produced
        REQUIRE(now.regionValue == valueAt(region, before.position));
        //  and the test means something only if that differs from reading the present position
        if (valueAt(region, now.position) != valueAt(region, before.position)) ++changes;
        before = now;
    }
    CHECK(changes >= 2);
}

TEST_CASE("after a restart the region source reads where the source starts, not where it last was") {
    /*  A source parked inside a region, driving gain: read as 0 on the first step it would snap to
        the outside value and fade back in, at every play, locate and loop. */
    Rig r;
    frontHalf(r);
    r.start();
    const Region region = asSet(r);
    const ControlFrame first = r.step();
    REQUIRE(valueAt(region, first.position) == 1.0);  // the orbit starts in front
    CHECK(first.regionValue == 1.0);                  // and the very first step says so

    //  play on until the source is behind, then restart: it is in front again, at once
    ControlFrame f = first;
    for (int i = 0; i < 2000 && valueAt(region, f.position) != 0.0; ++i) f = r.step();
    REQUIRE(valueAt(region, f.position) == 0.0);
    r.mod.restartTransport();
    r.control.restartTransport(false);
    CHECK(r.step().regionValue == 1.0);
}

TEST_CASE("the reseed reads the parameters as set, which is all there is before the first step") {
    //  The engine's destinations are zero until it has run once: a region resolved from them has
    //  no size, and a source inside it would read 0.
    Rig r;
    frontHalf(r);
    r.set(EncoderParam::MotionSpeed, 0.0f);
    r.start();
    CHECK(r.step().regionValue == 1.0);

    //  and it is the placed position that is read: turned half way round, the same source starts
    //  behind the listener, where front -- what a position never worked out would fall back to -- is wrong
    Rig behind;
    frontHalf(behind);
    behind.set(EncoderParam::MotionSpeed, 0.0f);
    behind.set(EncoderParam::TransformYaw, 180.0f);
    behind.start();
    CHECK(behind.step().regionValue == 0.0);
}

TEST_CASE("a region angle set back to zero takes its own turn with it, and only that axis") {
    Rig r;
    frontHalf(r);
    r.set(EncoderParam::Region1YawRate, 90.0f);
    r.set(EncoderParam::Region1PitchRate, 45.0f);
    r.set(EncoderParam::TransformYawRate, 90.0f);
    r.start();
    r.run(100);
    CHECK(r.control.regionRotation().yawRad() > 0.5);
    CHECK(r.control.regionRotation().pitchRad() > 0.2);
    const double placement = r.control.rotation().yawRad();
    CHECK(placement > 0.5);

    //  yaw only: bit 0. Catches a mask that clears everything, and one that clears the wrong axis.
    r.step(true, 0, 1 << 0);
    CHECK(r.control.regionRotation().yawRad() < 0.05);   // cleared, then one step of rate again
    CHECK(r.control.regionRotation().pitchRad() > 0.2);  // untouched
    //  Catches the region's mask reaching the placement's clock, which is the copy-paste to fear here.
    CHECK(r.control.rotation().yawRad() >= placement);

    //  and the placement's mask does not clear the region's
    const double regionPitch = r.control.regionRotation().pitchRad();
    r.step(true, 1 << 0, 0);  // the placement's yaw
    CHECK(r.control.rotation().yawRad() < 0.05);
    CHECK(r.control.regionRotation().pitchRad() >= regionPitch);
}

TEST_CASE("a region's rates turn it, hold while stopped, and go back to zero with the take") {
    Rig r;
    frontHalf(r);
    r.set(EncoderParam::Region1YawRate, 90.0f);
    r.start();
    r.run(200, false);
    CHECK(r.control.regionRotation().yawRad() == 0.0);
    const ControlFrame f = r.run(100);  // about 48 degrees: short of where a turn wraps
    const double turned = r.control.regionRotation().yawRad();
    CHECK(turned > 0.5);
    CHECK(r.control.regionRotation().pitchRad() == 0.0);
    CHECK(r.control.rotation().yawRad() == 0.0);  // the region's turn is not the placement's
    //  the turn is in the region that was read, one step behind the clock
    CHECK(f.region.yaw > 0.5);
    CHECK(f.region.yaw <= turned);

    r.control.restartTransport(true);
    CHECK(r.control.regionRotation().yawRad() == turned);
    r.control.restartTransport(false);
    CHECK(r.control.regionRotation().yawRad() == 0.0);
}

TEST_CASE("without a region there is no region source, and the side belongs to the use") {
    Rig none;
    frontHalf(none);
    none.useRegion = false;
    none.start();
    CHECK(none.run(50).regionValue == 0.0);

    Rig out;
    frontHalf(out);
    out.set(EncoderParam::Region1Side, 1.0f);  // outside (a parameter, not state)
    out.set(EncoderParam::MotionSpeed, 0.0f);
    out.start();
    CHECK(out.step().regionValue == 0.0);  // parked in front, which is now outside
}

/*  Every test above uses a Spot, whose size and softness are modulatable -- which is why a
 *  destination no cell can reach could read zero unnoticed. A band's thickness is the same read. */
TEST_CASE("a destination no cell can reach still carries its parameter, so a band keeps its shape") {
    Rig r;
    r.useRegion = true;
    r.state.regions[0].shape.kind = RegionKind::Band;
    r.set(EncoderParam::Region1Thickness, 40.0f);
    r.set(EncoderParam::MotionSpeed, 0.0f);
    r.start();

    const double snapped = r.step().region.thickness;
    CHECK(snapped == doctest::Approx(40.0 * kDeg2Rad));
    //  the step after the snap is the one that collapsed: the destination was never written, so it
    //  held the zero the restart left. Catches: skip NotModulatable ids and this reads 0.
    CHECK(r.step().region.thickness == doctest::Approx(snapped));
    CHECK(r.run(20).region.thickness == doctest::Approx(snapped));
}

// ---- a stereo input -------------------------------------------------------------------------------

namespace {
std::vector<double> gainsOf(const Encoder& e) { return {e.gains().begin(), e.gains().end()}; }
/// A sound field's energy: order n weighted by (2n + 1). Channel c is of order floor(sqrt(c)).
double energyOf(const Encoder& e) {
    double sum = 0.0;
    for (std::size_t c = 0; c < e.gains().size(); ++c)
        sum += (2.0 * std::floor(std::sqrt(static_cast<double>(c))) + 1.0) * e.gains()[c] * e.gains()[c];
    return sum;
}
/// A rig stepped once in `mode`, its frame snapped into a mid and a side encoder.
struct Aimed {
    Encoder mid, side;
    ControlFrame frame;
    explicit Aimed(Rig& rig, int steps = 40) {
        mid.prepare(3);
        side.prepare(3);
        rig.start();
        frame = rig.run(steps);
        frame.snap = true;
        applyFrame(mid, side, frame);
    }
};
}  // namespace

/*  The default matches the single-encoder form to the bit: in `sum` the mid is aimed by the
 *  one-encoder form the goldens hash, and the side is aimed at nothing, so `process` skips it and
 *  adds no zeros. Catches `sum` being routed through the pair target, and the side being left aimed
 *  at something. */
TEST_CASE("sum is one point, exactly as one encoder aims it, and the side is silent") {
    Rig rig;
    rig.set(EncoderParam::MotionSpeed, 90.0f);
    rig.set(EncoderParam::RenderWidth, 30.0f);
    rig.set(EncoderParam::RenderGain, -3.0f);
    Aimed two(rig);

    Encoder one;
    one.prepare(3);
    applyFrame(one, two.frame);
    CHECK(gainsOf(two.mid) == gainsOf(one));  // exactly: not approximately
    CHECK(two.side.silent());
    CHECK(two.frame.inputMode == InputMode::Sum);
}

/*  Catches the side not being a difference (it would put S into W, and a mono fold-down of the
 *  output would be the left channel alone), the mid moving when the mode does, and a spread of
 *  nothing not being `sum`. */
TEST_CASE("mid/side leaves the mid where sum has it and keeps the side out of W") {
    Rig sum, ms, closed;
    for (auto* r : {&sum, &ms, &closed}) {
        r->set(EncoderParam::MotionSpeed, 90.0f);
        r->set(EncoderParam::RenderWidth, 20.0f);
    }
    ms.set(EncoderParam::InputMode, 1.0f);
    ms.set(EncoderParam::InputSpread, 90.0f);
    closed.set(EncoderParam::InputMode, 1.0f);
    closed.set(EncoderParam::InputSpread, 0.0f);
    Aimed a(sum), b(ms), c(closed);

    CHECK(gainsOf(b.mid) == gainsOf(a.mid));
    CHECK(std::abs(b.side.gains()[0]) < 1e-12);
    CHECK(energyOf(b.side) > 0.1);
    CHECK(c.side.silent());  // no spread: exactly sum
}

/*  Stereo puts L at one point and R at the other, written in mid and side. A left-only input is
 *  M = S, so what it meets is mid + side -- which must be the left point alone, and mid - side the
 *  right alone. Catches the two weights' signs, the points being swapped, and the level: centred
 *  material (the mid) stays at one point's energy whatever lies between the two. */
TEST_CASE("stereo puts the left at one point and the right at the other, at one point's level") {
    Rig rig, none, sum;
    for (auto* r : {&rig, &none, &sum}) {
        r->set(EncoderParam::MotionSpeed, 90.0f);
        r->set(EncoderParam::RenderWidth, 15.0f);
    }
    rig.set(EncoderParam::InputMode, 2.0f);
    rig.set(EncoderParam::InputOffset, 0.2f);
    none.set(EncoderParam::InputMode, 2.0f);
    none.set(EncoderParam::InputOffset, 0.0f);
    Aimed two(rig), zero(none), one(sum);

    const auto onlyAt = [&](Vec3 where, double sign) {
        Encoder point;
        point.prepare(3);
        point.setTarget(where, two.frame.widthRad, 0, 2.0 * two.frame.pairGain);
        point.snap();
        double worst = 0.0;
        for (std::size_t c = 0; c < point.gains().size(); ++c)
            worst = std::max(worst, std::abs(two.mid.gains()[c] + sign * two.side.gains()[c] - point.gains()[c]));
        return worst;
    };
    CHECK(onlyAt(two.frame.left, +1.0) < 1e-12);   // L = M + S lands at the left point and nowhere else
    CHECK(onlyAt(two.frame.right, -1.0) < 1e-12);  // R = M - S at the right
    CHECK(length(two.frame.left - two.frame.right) > 0.3);

    CHECK(energyOf(two.mid) == doctest::Approx(energyOf(one.mid)).epsilon(1e-9));
    CHECK(gainsOf(zero.mid) == gainsOf(one.mid));  // no offset: exactly sum
    CHECK(zero.side.silent());
}

/*  A mono input is one point whatever the mode says -- a preset, an automation lane, a track that
 *  was stereo yesterday. `stereo` could still aim the mid at two points, landing a mono source on
 *  both while the window showed a greyed `sum`. Catches the mode being read without asking whether
 *  there is a side to place. */
TEST_CASE("a mono input is sum in every mode") {
    for (const float mode : {1.0f, 2.0f}) {
        Rig rig, sum;
        for (auto* r : {&rig, &sum}) {
            r->set(EncoderParam::MotionSpeed, 90.0f);
            r->set(EncoderParam::InputOffset, 0.3f);
            r->set(EncoderParam::InputSpread, 120.0f);
        }
        rig.set(EncoderParam::InputMode, mode);
        rig.stereoInput = false;
        Aimed a(rig), b(sum);
        CHECK(a.frame.inputMode == InputMode::Sum);
        CHECK(gainsOf(a.mid) == gainsOf(b.mid));
        CHECK(a.side.silent());
    }
}

/*  The channels belong to the path, not to the travel: a source sent round the other way keeps its
 *  left on the same side of it. Catches "left" being defined as "behind". */
TEST_CASE("reversing the motion does not swap a stereo input's channels") {
    Rig fwd, rev;
    for (auto* r : {&fwd, &rev}) {
        r->set(EncoderParam::InputMode, 2.0f);
        r->set(EncoderParam::InputOffset, 0.2f);
        r->set(EncoderParam::MotionSpeed, 0.0f);  // still, so both stand at the same place
    }
    rev.set(EncoderParam::MotionDirection, 1.0f);
    Aimed a(fwd, 3), b(rev, 3);
    CHECK(length(a.frame.left - b.frame.left) < 1e-9);
    CHECK(length(a.frame.right - b.frame.right) < 1e-9);
}

TEST_CASE("a weights kind is held to 0..1 in the encoder's reading") {
    /*  On the bus a custom region's ringing is the truth; here a region never leaves 0..1.
        Catches: drop the clamp in EncoderControl::step and the omni weight of 2 reads as 2. */
    Rig r;
    frontHalf(r);
    r.state.regions[0].shape.custom = true;
    r.state.regions[0].shape.weights[0] = 2.0;  // twice everywhere
    r.start();
    CHECK(valueAt(asSet(r), Vec3{1, 0, 0}) == doctest::Approx(2.0));  // the region itself says 2
    CHECK(r.step().regionValue == 1.0);                               // the reading says 1
    r.state.regions[0].shape.weights[0] = -1.0;
    for (int i = 0; i < 3; ++i) r.step();
    CHECK(r.step().regionValue == 0.0);
}

TEST_CASE("a stereo input's two points each wrap and bounce on their own") {
    /*  On an open path near its end, the offset is taken in the motion's phase, as displace is.
        Catches: the offset added to the folded position and clamped, which parks the right point on
        the end and turns both round together. */
    for (const float mode : {0.0f, 1.0f}) {  // wrap, ping-pong
        Rig r;
        r.state.trajectory.generator = GeneratorType::Arc;
        generatorDefaults(GeneratorType::Arc, r.state.trajectory.genParams);
        r.path.build(r.state.trajectory);
        r.centre = pathCentre(r.path.points());
        REQUIRE(!r.path.closed());
        r.set(EncoderParam::InputMode, 2.0f);
        r.set(EncoderParam::InputOffset, 0.2f);  // 0.1 either side
        r.set(EncoderParam::MotionSpeed, 0.0f);
        r.set(EncoderParam::MotionMode, mode);
        r.set(EncoderParam::MotionDisplace, 0.95f);  // the source near the end
        r.start();
        const auto f = r.step();
        const auto at = [&](double s) { return applyTransform(r.path.eval(s), r.centre, f.placement); };
        CAPTURE(mode);
        //  1e-6: the parameters are floats, so 0.95 and 0.1 are not exact
        CHECK(length(f.left - at(0.85)) < 1e-6);
        //  past the end: wrapped to the start, or bounced back from it -- never parked on it
        CHECK(length(f.right - at(mode == 0.0f ? 0.05 : 0.95)) < 1e-6);
        CHECK(length(f.right - at(1.0)) > 1e-3);
    }
}

TEST_CASE("displace set back to zero takes the distance travelled with it") {
    //  As an angle takes its turn. Catches: the bit ignored, so the source stays wherever the motion
    //  had carried it.
    Rig r;
    r.set(EncoderParam::MotionSpeed, 90.0f);
    r.start();
    ControlFrame f;
    for (int i = 0; i < 40; ++i) f = r.step();
    CHECK(f.s > 0.01);  // the motion has carried it along
    f = r.step(true, 8);
    CHECK(f.s < 0.01);  // back where displace alone (0) puts it, one step's travel on at most
}

TEST_CASE(
    "a modulated displace is folded by the movement mode: round a loop, back in ping-pong, stopped at the end once") {
    /*  Displace 0.9 pushed 0.3 on. Catches: the engine wraps displace whatever the mode, and on an open
        path the source jumps to 0.2 -- the far end -- where it should bounce back to 0.8 or stop at 1. */
    const auto landed = [](GeneratorType shape, MovementMode mode) {
        Rig r;
        r.state.trajectory.generator = shape;
        generatorDefaults(shape, r.state.trajectory.genParams);
        r.path.build(r.state.trajectory);
        r.centre = pathCentre(r.path.points());
        r.set(EncoderParam::MotionMode, static_cast<float>(mode));
        r.set(EncoderParam::MotionDisplace, 0.9f);
        //  an LFO held at its top: free, very slow, from the crest of a sine, 0 to 1
        r.set(EncoderParam::Lfo1Sync, 0.0f);
        r.set(EncoderParam::Lfo1Rate, 0.01f);
        r.set(EncoderParam::Lfo1Phase, 90.0f);
        r.set(EncoderParam::Lfo1Polarity, 0.0f);
        r.state.matrix.push_back({MatrixTab::Generators, 0, EncoderParam::MotionDisplace, 0.3});
        r.start();
        return r.run(60).s;
    };
    CHECK(landed(GeneratorType::Orbit, MovementMode::PingPong) ==
          doctest::Approx(0.2).epsilon(0.01));  // a loop comes round
    CHECK(landed(GeneratorType::Arc, MovementMode::Wrap) == doctest::Approx(0.2).epsilon(0.01));
    CHECK(landed(GeneratorType::Arc, MovementMode::PingPong) == doctest::Approx(0.8).epsilon(0.01));
    CHECK(landed(GeneratorType::Arc, MovementMode::Once) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("a path's settings are its keys: the same range, default and wrap as the generator's table") {
    /*  Two tables say one thing, and this holds them together. Catches: a key's range or default
        moved in one of them, two keys swapped, a whole-number setting given a key. */
    const auto& m = encodeParams();
    for (const auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                         GeneratorType::Spiral}) {
        const auto info = generatorParams(g);
        std::array<double, kMaxGenParams> defaults{};
        generatorDefaults(g, defaults);
        for (std::size_t i = 0; i < info.size(); ++i) {
            const ParamId id = pathParamId(g, static_cast<int>(i));
            INFO(std::string(name(g)), " ", std::string(info[i].name));
            const bool whole = info[i].step >= 1.0 && info[i].unit.empty();
            CHECK((id == kNoParamId) == whole);
            if (id == kNoParamId) continue;
            const auto& d = m[static_cast<int>(id)];
            CHECK(d.min == doctest::Approx(info[i].min));
            CHECK(d.max == doctest::Approx(info[i].max));
            CHECK(d.def == doctest::Approx(defaults[i]));
            CHECK(m.wrapsAt(static_cast<int>(id)) == info[i].wraps);
        }
    }
}

TEST_CASE("an LFO on a path's setting reshapes the path the source plays, step by step") {
    /*  An arc's length swept by an LFO: the source, parked at its end, moves with it as the engine
        sweeps the length. Catches: the path played is the one handed in, not rebuilt. */
    Rig r;
    r.state.trajectory.generator = GeneratorType::Arc;
    generatorDefaults(GeneratorType::Arc, r.state.trajectory.genParams);
    r.path.build(r.state.trajectory);
    r.centre = pathCentre(r.path.points());
    r.set(EncoderParam::MotionMode, static_cast<float>(MovementMode::Once));
    r.set(EncoderParam::MotionDisplace, 1.0f);  // at the arc's far end, where its length shows most
    r.set(EncoderParam::Lfo1Sync, 0.0f);
    r.set(EncoderParam::Lfo1Rate, 1.0f);
    r.state.matrix.push_back({MatrixTab::Generators, 0, EncoderParam::ArcLength, 0.3});
    r.start();
    double lengthLo = 1e9, lengthHi = -1e9;
    Vec3 first{};
    double moved = 0.0;
    for (int i = 0; i < 200; ++i) {
        const auto f = r.step();
        if (i == 0) first = f.position;
        moved = std::max(moved, arc(first, f.position));
        lengthLo = std::min(lengthLo, r.mod.destination(EncoderParam::ArcLength));
        lengthHi = std::max(lengthHi, r.mod.destination(EncoderParam::ArcLength));
    }
    INFO("length ran ", lengthLo, " .. ", lengthHi, "; the source moved ", moved * kRad2Deg, " degrees");
    CHECK(lengthHi - lengthLo > 60.0);
    CHECK(moved > 20.0 * kDeg2Rad);
}
