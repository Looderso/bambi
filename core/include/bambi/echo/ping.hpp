// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>

#include "bambi/echo/pass.hpp"
#include "bambi/math/vec3.hpp"

/*  The ping: where a tap carries a direction, for the editor to draw. Computed from the settings,
 *  never from audio. One pass does to a direction what echo/pass.hpp does to a field; blur widens
 *  rather than moves, so it is carried as a radius.
 */
namespace bambi {

/// A spiral is sampled in degrees of turn, not in passes, so a wide spin still draws as a curve.
inline constexpr double kPingStepDeg = 3.0;
inline constexpr int kPingMinSteps = 48, kPingMaxSteps = 480;

/// The skew two passes compose to: skews along one axis compose as Mobius maps.
constexpr double composeSkew(double a, double b) { return (a + b) / (1.0 + a * b); }

struct PingAt {
    double spinRad{0.0};
    double skew{0.0};
    double blurRad{0.0};
};

/// The loop after `passes` passes, which may be fractional.
PingAt loopAt(const TapSettings& tap, double passes);

/// Where a direction ends up that far into the loop.
Vec3 imageAt(const TapSettings& tap, Vec3 from, const PingAt& at);

/// `from` carried round the loop for `passes` passes, as a curve. `at[k]`, if given, is each
/// point's fraction of the whole loop.
void pingTrace(const TapSettings& tap, Vec3 from, int passes, std::vector<Vec3>& into,
               std::vector<float>* at = nullptr);

/// Start points for the flow lines: evenly spaced on the axis's equator, where the field stands
/// before the first pass.
inline constexpr int kFlowSpokes = 12;
void flowSeeds(const TapSettings& tap, std::array<Vec3, kFlowSpokes>& into);

/// Where pass j lands.
struct PingBead {
    Vec3 direction{1.0, 0.0, 0.0};
    double gain{1.0};     ///< linear: level * feedback^(j-1)
    double blurRad{0.0};  ///< blur compounds as the square root of the passes
    int index{1};         ///< j, from 1
};

/// One bead per pass. `level` and `feedback` are linear.
std::vector<PingBead> pingSpiral(const TapSettings& tap, Vec3 from, int passes, double level = 1.0,
                                 double feedback = 0.5);

/// Where one whole pass carries a direction.
inline Vec3 pingStep(const TapSettings& tap, Vec3 from) {
    return imageAt(tap, from, {tap.spinRad, tap.skew, tap.blurRad});
}

}  // namespace bambi
