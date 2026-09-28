// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"

namespace bambi {

/*  Ambisonic encoder: one signal in, (order+1)^2 channels out. Gains ramp per sample so a moving
 *  source does not click; width is a spherical-cap weighting per order. Real-time safe after
 *  prepare().
 */
class Encoder {
public:
    /// Allocates. Call before processing, never during.
    void prepare(int order);

    /*  Aim the encoder. `rampSamples` is how long the move takes; 0 ramps across the next process()
     *  call. Pass it when targets come on a control grid, so the output does not depend on the host's
     *  block size. `linearGain` is ramped with the direction, so level changes do not step. */
    void setTarget(Vec3 direction, double widthRad, int rampSamples = 0, double linearGain = 1.0);

    /*  Two points in one target, `weightA * Y(a) + weightB * Y(b)`, ramped as setTarget ramps. A
     *  stereo side encoder aims at (+1, -1), which cancels in W. Kept separate from setTarget so the
     *  single-point path stays bit-identical. */
    void setTargetPair(Vec3 a, double weightA, Vec3 b, double weightB, double widthRad, int rampSamples = 0,
                       double linearGain = 1.0);

    /// Aimed at nothing and already there: process() would add zeros.
    bool silent() const;

    /// Snap to the target without a ramp — for the first block, or after a seek.
    void snap();

    /// mono `in` -> `out[ch]`, adding into out. Channels beyond numChannels() are left alone, so an
    /// oversized bus is handled by the caller, not silently zeroed here.
    void process(const float* in, std::span<float* const> out, int numSamples);

    int order() const { return order_; }
    int numChannels() const { return numChannels(order_); }
    static constexpr int numChannels(int order) { return (order + 1) * (order + 1); }

    /// Current (already-ramped) gains, for tests and metering.
    std::span<const double> gains() const { return current_; }

private:
    int order_{1};
    std::vector<double> start_;  ///< gains when the current ramp began
    int rampPos_{0}, rampLen_{0};
    std::vector<double> current_;
    std::vector<double> target_;
    std::vector<double> capW_;
    std::vector<double> y_;
    std::vector<double> yB_;  ///< the second point of a pair
};

/*  A source's left and right, `spreadRad` apart along the level great circle through it, so the
 *  width holds as the source climbs. Within ten degrees of a pole the spread closes to nothing
 *  rather than flipping sides. */
struct StereoPair {
    Vec3 left, right;
};
StereoPair stereoPairAbout(Vec3 direction, double spreadRad);

/*  The energy overlap of two encodes of one width: the cosine between their gain vectors, order n
 *  weighted by (2n + 1). Two points sharing a signal keep one point's level at 1/sqrt(2(1 + overlap)).
 */
double encodeOverlap(Vec3 a, Vec3 b, double widthRad, int order);

}  // namespace bambi
