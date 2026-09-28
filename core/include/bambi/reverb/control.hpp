// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <span>

#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/region/shape.hpp"
#include "bambi/reverb/engine.hpp"
#include "bambi/reverb/params.hpp"

/*  Reverb's settings, as a user sets them, to the frame its engine plays.
 *
 *  `ReverbSettings` is in the units of the controls -- metres, seconds, decibels, degrees, hertz --
 *  and knows nothing of parameter ids; `resolveReverb()` is the one place those units become the
 *  engine's: radians, linear gains, line counts. `ReverbResolver` wraps it to hold each region's
 *  accumulated turn between control steps, since a region's rates turn it over time.
 */
namespace bambi {

/// The quality switch while playing. The tail's line count is the lever; its order is not.
enum class ReverbQuality { Efficient, Realistic };

/// The factory presets, tuned by ear.
enum class ReverbPreset { Ambience, Room, Chamber, Hall, LargeHall, Cathedral };

/// One region slot: shape and counts are state, angle/size/side are parameters, amount is its own.
struct ReverbRegionSettings {
    RegionShape shape{RegionKind::Everywhere, 4, 6};
    RegionSide side{RegionSide::Inside};
    RegionSettingsDeg deg;
    RegionRatesDeg rates;  ///< degrees a second; the resolver holds the turn
    double amount{1.0};
};

struct ReverbSettings {
    // ---- the room ---------------------------------------------------------------------------------
    //  Ranges and defaults are the parameter list's (reverb/params.hpp).
    double sizeMetres{reverbDefault("room.size")};  ///< the box's width, the rest following its shape
    RoomShape shape{RoomShape::Hall};
    double decaySeconds{reverbDefault("room.decay")};       ///< RT60 at mid
    double tone{reverbDefault("room.tone")};                ///< how much faster the highs die and longer the lows ring
    double roughness{reverbDefault("room.roughness")};      ///< what scatters per bounce, and the diffusion
    double distanceMetres{reverbDefault("room.distance")};  ///< where sources are assumed to be

    /// False ties both regions' turns to playback, held while stopped; true runs them all the time.
    bool ratesContinue{reverbDefault("rates.retrigger") > 0.0};

    // ---- output and input -------------------------------------------------------------------------
    /// Independent, not a mix: wet is the room's level, dry the source's distance; dry opens at unity.
    double wetDb{reverbDefault("output.wet")};
    double dryDb{reverbDefault("output.dry")};                 ///< its minimum is off
    double preDelayTrimMs{reverbDefault("output.pre_delay")};  ///< added to the geometry's own pre-delay
    double lowCutHz{reverbDefault("input.low_cut")};
    double highCutHz{reverbDefault("input.high_cut")};

    // ---- the slots ----------------------------------------------------------------------------------
    ReverbRegionSettings send;      ///< what of the field enters the room
    ReverbRegionSettings returnTo;  ///< where what the room made is put back

    // ---- quality --------------------------------------------------------------------------------
    ReverbQuality quality{ReverbQuality::Realistic};
    RenderQuality renderQuality{RenderQuality::Realistic};

    /// The factory presets, each setting the room and its cuts and leaving everything else alone.
    static ReverbSettings preset(ReverbPreset p);
};

/// Writes a preset into a host's parameter values via `set(position, normalised value)`, one call per
/// key the preset touches (the room and its cuts only).
void applyPreset(const ParamManifest& m, ReverbPreset p, const std::function<void(int at, float normalised)>& set,
                 RoomState& room);

/// Reverb's settings from a host's parameter values; region shapes and `renderQuality` are state,
/// brought by the caller.
ReverbSettings reverbSettingsFrom(const ParamManifest& m, std::span<const float> values, RoomShape shape,
                                  const RegionShape& sendShape, const RegionShape& returnShape,
                                  RenderQuality renderQuality);

/// One control step, with the region turns given rather than held. Real-time safe: bounded work, no
/// allocation.
ReverbFrame resolveReverb(const ReverbSettings& settings, bool offline, const RotationClock& sendTurn = {},
                          const RotationClock& returnTurn = {}) noexcept;

/// What a control step is given that is not a parameter.
struct ReverbControlInput {
    std::span<const double> self;       ///< the six features of the input field
    std::span<const double> sidechain;  ///< the same of the sidechain; empty when there is none
    Transport transport;
    double dt{0.0};                        ///< seconds this step covers
    bool offline{false};                   ///< what the host says about the render it is doing
    RegionShape sendShape, returnShape;    ///< kinds and counts are state
    RoomShape roomShape{RoomShape::Hall};  ///< the room's, state too
    /// Bit per axis (1 yaw, 2 pitch, 4 roll) per slot: clears that slot's accumulated turn.
    int zeroRegionTurns[2]{0, 0};
    RenderQuality renderQuality{RenderQuality::Realistic};  ///< state, not automatable
};

/// The same step, holding what has to be held between steps: each slot's accumulated turn.
class ReverbResolver {
public:
    /// A render must repeat, so a play start, locate or loop resets the turns to nothing.
    void reset() noexcept {
        sendTurn_.reset();
        returnTurn_.reset();
    }

    /// As `reset()`, unless the user has set the turns to continue regardless.
    void restartTransport(bool ratesContinue) noexcept {
        if (!ratesContinue) reset();
    }

    ReverbFrame resolve(const ReverbSettings& settings, bool offline, double sampleRate, bool playing = true) noexcept;

    /// One whole control step: runs modulation, reads every parameter back modulated, and resolves
    /// from those. Real-time safe: no allocation; `modulated_` is a member so kMaxParams floats need
    /// not grow on the stack each step.
    ReverbFrame step(ModulationEngine& mod, const ParamManifest& m, const std::array<float, kMaxParams>& base,
                     const ReverbControlInput& in, double sampleRate) noexcept;

    /// What the rates have turned each region by, for the editor to draw the audio thread's actual state.
    const RotationClock& sendTurn() const noexcept { return sendTurn_; }
    const RotationClock& returnTurn() const noexcept { return returnTurn_; }

private:
    RotationClock sendTurn_, returnTurn_;
    std::array<float, kMaxParams> modulated_{};  ///< step()'s scratch: every parameter after modulation
};

/// How many tail lines a setting asks for, which is the whole of what quality scales.
int tailLinesFor(ReverbQuality q) noexcept;

}  // namespace bambi
