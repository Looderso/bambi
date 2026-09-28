// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/encode/control.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"

namespace bambi {
namespace {

constexpr ParamSpec kSpread = encoderParamSpec("input.spread");
constexpr ParamSpec kOffset = encoderParamSpec("input.offset");

float baseParam(std::span<const float> base, ParamId id) {
    const auto i = static_cast<std::size_t>(id);
    return i < base.size() ? base[i] : parameter(id).def;
}

}  // namespace

EncoderControl::EncoderControl() {
    live_.reserveParametric();
    scratch_.reserveParametric();
}

void EncoderControl::reset() {
    clock_.reset(0.0);
    rotation_.reset();
    regionRotation_.reset();
    snapNext_ = true;
}

void EncoderControl::restartTransport(bool motionContinues) {
    if (!motionContinues) {
        clock_.reset(0.0);
        rotation_.reset();  // an integrated rotation belongs to the take that started it
        regionRotation_.reset();
    }
    snapNext_ = true;
}

ControlFrame EncoderControl::step(ModulationEngine& mod, std::span<const float> base, const ControlInput& in) noexcept {
    const auto mode = static_cast<MovementMode>(
        std::clamp(static_cast<int>(std::lround(baseParam(base, EncoderParam::MotionMode))), 0, 2));

    //  The path this step plays: the one handed in, until a parametric one is rebuilt from the engine below.
    const Trajectory* path = in.path;
    Vec3 centre = in.centre;

    //  Where the source is for a given reading of the parameters, with the clocks as they stand.
    const auto place = [&](const auto& get, ControlFrame& f) {
        f.placement.yawRad = get(EncoderParam::TransformYaw) * kDeg2Rad + rotation_.yawRad();
        f.placement.pitchRad = get(EncoderParam::TransformPitch) * kDeg2Rad + rotation_.pitchRad();
        f.placement.rollRad = get(EncoderParam::TransformRoll) * kDeg2Rad + rotation_.rollRad();
        f.placement.extent = get(EncoderParam::TransformExtent);
        if (path == nullptr) return;
        f.s = clock_.sAt(get(EncoderParam::MotionDisplace), mode, path->closed());
        f.position = applyTransform(path->eval(f.s), centre, f.placement);
    };
    const auto regionFrom = [&](const auto& get) {
        RegionSettingsDeg set;
        set.yaw = get(EncoderParam::Region1Yaw), set.pitch = get(EncoderParam::Region1Pitch),
        set.roll = get(EncoderParam::Region1Roll);
        set.softness = get(EncoderParam::Region1Softness), set.size = get(EncoderParam::Region1Size);
        set.bandElevation = get(EncoderParam::Region1BandElevation),
        set.thickness = get(EncoderParam::Region1Thickness);
        set.fill = get(EncoderParam::Region1Fill), set.dotSize = get(EncoderParam::Region1DotSize);
        set.coverage = get(EncoderParam::Region1Coverage), set.contrast = get(EncoderParam::Region1Contrast);
        set.detail = get(EncoderParam::Region1Detail), set.evolve = get(EncoderParam::Region1Evolve);
        //  Which side passes is a parameter of the region, so it is modulated and
        //  automated like the rest of the block rather than being fixed in state.
        const RegionSide side = get(EncoderParam::Region1Side) > 0.5 ? RegionSide::Outside : RegionSide::Inside;
        return resolveRegion(in.region->shape, side, set, regionRotation_.yawRad(), regionRotation_.pitchRad(),
                             regionRotation_.rollRad());
    };
    const auto asSet = [&](ParamId id) { return static_cast<double>(baseParam(base, id)); };

    /*  Displace is folded by the movement mode, as the motion is: past the end of a loop it comes
        round again; on a path bounced back and forth it bounces, there-and-back being two laps of
        phase; on a path run once it stops at the end. Its period is set before the engine runs, so a
        modulated displace is never clamped or made to jump from one end of an open path to the other. */
    const bool loops = path == nullptr || path->closed() || mode == MovementMode::Wrap;
    mod.setPeriod(EncoderParam::MotionDisplace, loops ? 1.0 : mode == MovementMode::PingPong ? 2.0 : 0.0);
    const auto dest = [&mod](ParamId id) { return mod.destination(id); };

    //  The region source, before the engine runs: from the step before, or -- when there is none --
    //  from the parameters as set. The engine's destinations still hold the step before's values
    //  here, which is the same step the remembered position belongs to.
    ControlFrame out;
    if (in.region != nullptr) {
        Vec3 where = previous_;
        if (snapNext_) {
            ControlFrame seed;
            place(asSet, seed);
            where = seed.position;
            out.region = regionFrom(asSet);
        } else {
            out.region = regionFrom(dest);
        }
        //  A weights kind rings outside 0..1 on the bus; here, where a region never leaves 0..1, it is
        //  held there.
        out.regionValue = std::clamp(valueAt(out.region, where), 0.0, 1.0);
    }
    const std::array<double, 1> regionValues{out.regionValue};
    mod.process(in.self, in.sidechain, regionValues, in.transport, in.dt);

    /*  A parametric path is rebuilt from its settings as the engine leaves them, when they have moved
        since the last build -- in room reserved at construction, so nothing allocates. The source keeps its
        place as a fraction of the lap, so a shape that changes smoothly carries it smoothly. */
    if (in.shape != nullptr && in.shape->kind == TrajectoryKind::Parametric) {
        TrajectoryState now;
        now.kind = TrajectoryKind::Parametric;
        now.generator = in.shape->generator;
        now.genParams = in.shape->genParams;  // the whole-number settings, as set
        pathSettingsFrom(now, mod);
        if (!haveLive_ || now.generator != built_.generator || now.genParams != built_.genParams) {
            live_.build(now, scratch_);
            liveCentre_ = pathCentre(live_.points());
            built_.generator = now.generator;
            built_.genParams = now.genParams;
            haveLive_ = true;
        }
        path = &live_;
        centre = liveCentre_;
    }

    out.speed = dest(EncoderParam::MotionSpeed);
    if (baseParam(base, EncoderParam::MotionDirection) > 0.5f) out.speed = -out.speed;  // "forward,reverse"

    //  Set to restart, the motion is linked to playback: stopped, the source and the placement's turn
    //  hold where they are. Set to continue, they run all the time.
    const bool continues = baseParam(base, EncoderParam::RatesRetrigger) > 0.5f;
    const double motionDt = in.transport.playing || continues ? in.dt : 0.0;
    const double length = path != nullptr ? path->lengthRad() : 0.0;
    //  Displace set back to zero by hand takes the distance travelled with it, as an angle takes its
    //  turn: the source goes back to where displace alone puts it.
    if ((in.zeroTurns & 8) != 0) clock_.reset();
    clock_.advance(out.speed, length, motionDt);

    //  An angle set back to zero by hand takes its accumulated turn with it.
    zeroTurns(rotation_, in.zeroTurns);
    zeroTurns(regionRotation_, in.zeroRegionTurns);

    //  The rates are integrated here, the way the motion clock integrates speed, and added to the
    //  placement -- the parameters keep saying what the user set.
    rotation_.advance(dest(EncoderParam::TransformYawRate), dest(EncoderParam::TransformPitchRate),
                      dest(EncoderParam::TransformRollRate), motionDt);
    regionRotation_.advance(dest(EncoderParam::Region1YawRate), dest(EncoderParam::Region1PitchRate),
                            dest(EncoderParam::Region1RollRate), motionDt);

    place(dest, out);
    previous_ = out.position;

    //  Where a stereo input's two points are. The mode is as set: it takes no modulation.
    out.inputMode = !in.stereoInput
                        ? InputMode::Sum
                        : static_cast<InputMode>(std::clamp(
                              static_cast<int>(std::lround(baseParam(base, EncoderParam::InputMode))), 0, 2));
    out.left = out.right = out.position;
    if (out.inputMode == InputMode::MidSide) {
        const auto pair = stereoPairAbout(
            out.position, std::clamp(dest(EncoderParam::InputSpread), kSpread.min, kSpread.max) * kDeg2Rad);
        out.left = pair.left;
        out.right = pair.right;
    } else if (out.inputMode == InputMode::Stereo && path != nullptr) {
        /*  Either side of where the source is, as two travellers: the offset is taken in the motion's
            phase before the movement mode folds it, as displace is, so each point wraps or bounces on
            its own instead of both being clamped and stuck together at an open path's end. Reversing
            the motion does not swap the channels: the left trails in phase whichever way it runs. The
            source's own position stays the middle, which is what the region source reads and what the
            scene selects. */
        const double half = 0.5 * std::clamp(dest(EncoderParam::InputOffset), kOffset.min, kOffset.max);
        const double displace = dest(EncoderParam::MotionDisplace);
        const bool closed = path->closed();
        out.left = applyTransform(path->eval(clock_.sAt(displace - half, mode, closed)), centre, out.placement);
        out.right = applyTransform(path->eval(clock_.sAt(displace + half, mode, closed)), centre, out.placement);
    }
    out.widthRad = dest(EncoderParam::RenderWidth) * kDeg2Rad;
    //  Two points that are one point are a half each, said outright: `dot` of a direction with itself
    //  can fall a bit short of 1, and "offset 0 is exactly sum" is a promise, not an approximation.
    const bool onePoint = out.left.x == out.right.x && out.left.y == out.right.y && out.left.z == out.right.z;
    if (out.inputMode == InputMode::Stereo && !onePoint)
        out.pairGain = 1.0 / std::sqrt(2.0 * (1.0 + encodeOverlap(out.left, out.right, out.widthRad, in.order)));
    out.gainDb = dest(EncoderParam::RenderGain);
    out.trimDb = dest(EncoderParam::InputTrim);

    out.snap = snapNext_;
    snapNext_ = false;
    return out;
}

void applyFrame(Encoder& encoder, const ControlFrame& frame) {
    //  Gain rides the encoder's ramp with the direction and the width, not a multiply after it.
    const double gain = std::pow(10.0, frame.gainDb / 20.0);
    if (frame.snap) {
        encoder.setTarget(frame.position, frame.widthRad, 0, gain);
        encoder.snap();
    } else {
        encoder.setTarget(frame.position, frame.widthRad, kControlHop, gain);
    }
}

void applyFrame(Encoder& mid, Encoder& side, const ControlFrame& frame) {
    const double gain = std::pow(10.0, frame.gainDb / 20.0);
    const int ramp = frame.snap ? 0 : kControlHop;
    switch (frame.inputMode) {
        case InputMode::Sum:
            applyFrame(mid, frame);  // the one-encoder form, to the bit
            side.setTargetPair(frame.position, 0.0, frame.position, 0.0, frame.widthRad, ramp, gain);
            if (frame.snap) side.snap();
            return;
        case InputMode::MidSide:
            applyFrame(mid, frame);
            side.setTargetPair(frame.left, 1.0, frame.right, -1.0, frame.widthRad, ramp, gain);
            break;
        case InputMode::Stereo:
            //  L at one point and R at the other, written in mid and side: L = M + S, R = M - S
            mid.setTargetPair(frame.left, frame.pairGain, frame.right, frame.pairGain, frame.widthRad, ramp, gain);
            side.setTargetPair(frame.left, frame.pairGain, frame.right, -frame.pairGain, frame.widthRad, ramp, gain);
            if (frame.snap) mid.snap();
            break;
    }
    if (frame.snap) side.snap();
}

}  // namespace bambi
