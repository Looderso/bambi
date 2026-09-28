// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>

#include "bambi/encode/encoder.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/trajectory.hpp"
#include "bambi/region/shape.hpp"

/*  The encoder's control step: from what the modulation engine says this step to where the source
 *  is, how wide, and how loud. Lives here rather than in the plugin's processor so live playback and
 *  an offline bounce run exactly the same step. Everything about when a step happens -- the control
 *  grid, locate detection, which step a MIDI note belongs to, adopting a patch -- stays with the
 *  caller, since that depends on the host.
 */
namespace bambi {

/// Samples between control steps. Fixed, so that a result never depends on the host's block size.
inline constexpr int kControlHop = 256;

struct ControlInput {
    std::span<const double> self;       ///< the six features of the input
    std::span<const double> sidechain;  ///< the same of the sidechain; empty when there is none
    Transport transport;
    double dt{0.0};  ///< seconds this step covers
    const Trajectory* path{nullptr};
    Vec3 centre{0.0, 0.0, 1.0};  ///< pathCentre of the path's points
    /// The path as set. A parametric one is rebuilt here from its settings whenever they move, and
    /// played in place of `path`; a custom chain plays `path` as it is.
    const TrajectoryState* shape{nullptr};
    int zeroTurns{0};                    ///< set back to zero by hand: 1 yaw, 2 pitch, 4 roll, 8 the distance travelled
    int zeroRegionTurns{0};              ///< the same, for the region's own three rates
    const RegionEntry* region{nullptr};  ///< what the region is and which side is used; none reads 0
    int order{3};                        ///< the ambisonic order; two points sharing a signal overlap by it
    /// A mono input has no side, and is one point whatever the mode parameter says: without this,
    /// `stereo` still aimed the mid at two points, landing a mono source on both.
    bool stereoInput{true};
};

/*  What a stereo input puts on the sphere. The two encoders are always fed the input's mid and its
 *  side; the mode only changes what each is aimed at, so switching it is a ramp, never a signal
 *  change.
 *
 *      sum        mid at the source; side at nothing.
 *      mid/side   mid at the source; side at +left and -right of it, `spread` apart, cancelling in W.
 *      stereo     left and right as two points `offset` apart either side of the source; mid at
 *                 their sum, side at their difference (L at one point, R at the other).
 *
 *  Spread 0 and offset 0 are both exactly `sum`. */
enum class InputMode { Sum, MidSide, Stereo };

/// What one step decided. `snap` says the encoder should jump there rather than ramp: the first
/// step after anything that broke continuity.
struct ControlFrame {
    Vec3 position{1.0, 0.0, 0.0};
    double s{0.0};  ///< where on the path, 0..1
    double widthRad{0.0};
    double gainDb{0.0};
    double trimDb{0.0};
    double speed{0.0};        ///< deg/s along the path, signed by direction
    PathTransform placement;  ///< as applied: the set angles plus what the rates have turned
    bool snap{false};
    Region region;            ///< as it was read this step: settings, modulation and turn included
    double regionValue{0.0};  ///< what the region source was given, before its amount

    InputMode inputMode{InputMode::Sum};
    Vec3 left{1.0, 0.0, 0.0}, right{1.0, 0.0, 0.0};  ///< the two points; both `position` in sum
    /*  What each of two points sharing a signal is scaled by, so that what they add up to stays at
     *  one point's level: 1/sqrt(2(1 + overlap)). 0.5 when they coincide, which is exactly `sum`. */
    double pairGain{0.5};
};

class EncoderControl {
public:
    /// Allocates: room for the largest parametric path, so a rebuild in a step allocates nothing.
    EncoderControl();

    /// Preparing to play: every clock to zero, and the next step snaps.
    void reset();

    /*  A transport start, a locate, a loop's jump. The next step snaps. The source's place on its
     *  path and the placement's integrated turn go back to zero unless the motion is set to continue
     *  (rates.retrigger), in which case a bounce no longer repeats what was heard -- by choice. */
    void restartTransport(bool motionContinues);

    /// The next step snaps and nothing else changes: a state was loaded under it.
    void requestSnap() { snapNext_ = true; }

    /*  One control step. Runs the engine, then reads it. `base` is the plugin's parameters as set --
     *  kNumEncoderParams of them -- for the three the matrix cannot move: direction, mode and retrigger.
     *  Real-time safe: no allocation, no lock. */
    ControlFrame step(ModulationEngine& mod, std::span<const float> base, const ControlInput& in) noexcept;

    const RotationClock& rotation() const { return rotation_; }
    const RotationClock& regionRotation() const { return regionRotation_; }
    const MotionClock& motion() const { return clock_; }

private:
    /*  The region source reads where the source was: reading where it is would be a loop with no
     *  first step, so it is always one step late. When continuity has just broken there is no step
     *  before, and the place is worked out from the parameters as set, so a source parked inside a
     *  region starts inside it rather than fading in at every play, locate and loop. */
    MotionClock clock_;
    RotationClock rotation_;
    RotationClock regionRotation_;  ///< the region's rates, integrated and held exactly as the placement's are
    Vec3 previous_{1.0, 0.0, 0.0};
    bool snapNext_{true};

    /*  The path as it plays, when it is parametric: rebuilt from the engine's settings when they move,
        in room reserved at construction. `built_` is what it was last built from. */
    Trajectory live_;
    PathScratch scratch_;
    TrajectoryState built_{};
    Vec3 liveCentre_{0.0, 0.0, 1.0};
    bool haveLive_{false};
};

/// Hand a frame to the encoder -- where, how wide, how loud -- jumping there if the frame snaps and
/// otherwise ramping over one control hop.
void applyFrame(Encoder& encoder, const ControlFrame& frame);

/*  The same, for a stereo input: `mid` is fed (L+R)/2 and `side` (L-R)/2, always. In `sum` the mid
 *  is aimed exactly as the one-encoder form aims it, and the side at nothing -- so the path every
 *  golden hashes is the path that runs. */
void applyFrame(Encoder& mid, Encoder& side, const ControlFrame& frame);

}  // namespace bambi
