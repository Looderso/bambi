// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>

#include "bambi/echo/engine.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/echo/timing.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/region/shape.hpp"

/*  Echo's control layer: EchoSettings, in the units of the controls, become the EchoFrame the
 *  engine plays. EchoResolver::resolve() is the only place they become samples, gains and radians.
 */
namespace bambi {

/// One tap. Ranges and defaults are the parameter list's; a fresh tap is kEchoTapDefaults[k].
struct EchoTapSettings {
    bool on{false};
    TapTiming timing;
    double levelDb{0.0};
    double feedbackDb{0.0};  ///< per pass
    double axisAzimuthDeg{0.0};
    double axisElevationDeg{0.0};  ///< exactly 90 is the pole, and skips the turns
    double spinDeg{0.0};           ///< per pass
    double skew{0.0};              ///< per pass
    double blurDeg{0.0};           ///< per pass
    double lowCutHz{0.0};
    double highCutHz{0.0};
};

/// A fresh instance's tap, from the parameter list. `tap` is "tap1".."tap4".
constexpr EchoTapSettings echoTapDefaults(std::string_view tap) {
    const auto def = [tap](std::string_view suffix) { return echoParamSpec(tap, suffix).def; };
    EchoTapSettings t;
    t.on = def(".on") > 0.0;
    t.timing.synced = def(".synced") > 0.0;
    t.timing.steps = static_cast<int>(def(".steps"));
    t.timing.offsetSteps = static_cast<int>(def(".offset_steps"));
    t.timing.swing = def(".swing");
    t.timing.ms = def(".ms");
    t.timing.offsetMs = def(".offset_ms");
    t.levelDb = def(".level");
    t.feedbackDb = def(".feedback");
    t.axisAzimuthDeg = def(".az");
    t.axisElevationDeg = def(".el");
    t.spinDeg = def(".spin");
    t.skew = def(".skew");
    t.blurDeg = def(".blur");
    t.lowCutHz = def(".low_cut");
    t.highCutHz = def(".high_cut");
    return t;
}

inline constexpr std::array<EchoTapSettings, kEchoTaps> kEchoTapDefaults{
    echoTapDefaults("tap1"), echoTapDefaults("tap2"), echoTapDefaults("tap3"), echoTapDefaults("tap4")};

struct EchoSettings {
    std::array<EchoTapSettings, kEchoTaps> taps{kEchoTapDefaults};
    RegionShape sendShape{RegionKind::Everywhere, 4, 6};
    RegionSettingsDeg send;
    RegionSide sendSide{RegionSide::Inside};
    double sendAmount{echoDefault("mod.amount.region1")};
    RegionRatesDeg sendRates;  ///< degrees a second
    /// False: the region's turn holds while stopped and clears at a restart. True: it always runs.
    bool ratesContinue{echoDefault("rates.retrigger") > 0.0};

    /// Two independent output levels rather than a mix. Their minimum is exact silence.
    double wetDb{echoDefault("output.wet")};
    double dryDb{echoDefault("output.dry")};

    static EchoSettings defaults() { return {}; }
};

/// Settings from parameter values, read by key. The send's shape is plugin state and passed in.
EchoSettings echoSettingsFrom(const ParamManifest& m, std::span<const float> values, const RegionShape& sendShape);

/// Everything a control step needs that is not a parameter.
struct EchoControlInput {
    std::span<const double> self;       ///< the input field's features
    std::span<const double> sidechain;  ///< empty when there is none
    Transport transport;
    double dt{0.0};  ///< seconds this step covers
    int order{3};
    RegionShape sendShape;
    /// Axes whose accumulated turn to clear, because the user zeroed the angle: 1 yaw, 2 pitch, 4 roll.
    int zeroRegionTurns{0};
};

class EchoResolver {
public:
    /// Forgets the held periods and the region's turn, so a render repeats exactly.
    void reset() noexcept {
        held_.fill(0);
        sendTurn_.reset();
    }

    /// A play start, locate or loop. The region's turn survives only if it is set to continue.
    void restartTransport(bool ratesContinue) noexcept {
        held_.fill(0);
        if (!ratesContinue) sendTurn_.reset();
    }

    /// One control step. Real-time safe. A period is held until the tap's time really moves, so
    /// a tempo jittering in its last bit does not move it.
    EchoFrame resolve(const EchoSettings& settings, double bpm, double sampleRate, int order,
                      bool playing = true) noexcept;

    /// How far the rates have turned the send region, for the editor to draw what is applied.
    const RotationClock& sendTurn() const noexcept { return sendTurn_; }

    /*  The plugin's whole control step: run modulation on `base` (the host's values), then resolve
     *  the modulated parameters. Real-time safe. The matrix's region source reads zero: an effect
     *  has no source position to read the region at. */
    EchoFrame step(ModulationEngine& mod, const ParamManifest& m, const std::array<float, kMaxParams>& base,
                   const EchoControlInput& in, double sampleRate) noexcept;

private:
    std::array<int, kEchoTaps> held_{};
    RotationClock sendTurn_;
    std::array<float, kMaxParams> modulated_{};  ///< step()'s scratch, a member to keep it off the stack
};

}  // namespace bambi
