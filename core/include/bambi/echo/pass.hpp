// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>

#include "bambi/echo/warp.hpp"
#include "bambi/math/vec3.hpp"

/*  One pass of one tap, what a repeat goes through each time round the loop:
 *
 *      turn the axis to the pole -> skew -> spin -> turn back -> blur -> band
 *
 *  Skew and spin are cheap only about the pole, hence the turns; an axis that is the pole skips
 *  them. Blur is one gain per order.
 */
namespace bambi {

struct TapSettings {
    Vec3 axis{0.0, 0.0, 1.0};
    bool axisIsPole{true};  ///< stated by the caller, not found by comparing floats
    double skew{0.0};       ///< positive gathers toward the axis's tip
    double spinRad{0.0};    ///< per pass; counter-clockwise seen from the axis's tip
    double blurRad{0.0};    ///< cap half-angle
    double lowCutHz{20.0};
    double highCutHz{20000.0};
};

struct TapTransform {
    int order{0};
    bool plain{true};  ///< the axis is the pole: no turns
    bool skewed{false};
    std::vector<float> toPole, fromPole;  ///< column-major rotation blocks (dsp/field.hpp)
    std::vector<float> warp;              ///< column-major, laid out by warpOffset
    std::vector<float> cosM, sinM;        ///< of m times the spin
    std::vector<float> gains;             ///< the blur, one per order
    double lowpass{1.0}, highpass{0.0};   ///< one-pole coefficients
};

/// Two one-pole states per channel, in double to keep a long tail's error low.
struct BandState {
    std::vector<double> low, high;
    void prepare(int order);
    void clear() noexcept;
};

/// Scratch space for building and running passes, shared by an engine's taps.
class PassWork {
public:
    void prepare(int order, int maxFrames);
    int maxFrames() const { return maxFrames_; }

private:
    friend void buildTap(const TapSettings&, double, PassWork&, TapTransform&) noexcept;
    friend void onePass(const TapTransform&, BandState&, PassWork&, float*, int) noexcept;
    int order_{0}, maxFrames_{0};
    WarpBuilder warp_;
    std::vector<double> rotation_, warpBlocks_, caps_;
    std::vector<float> a_, b_;
};

/// Allocates; buildTap does not.
void prepareTap(TapTransform& tap, int order);

/// Real-time safe.
void buildTap(const TapSettings& settings, double sampleRate, PassWork& work, TapTransform& tap) noexcept;

/// In place, `frames` <= work.maxFrames(). Bit-identical however the frames are split.
void onePass(const TapTransform& tap, BandState& band, PassWork& work, float* data, int frames) noexcept;

}  // namespace bambi
