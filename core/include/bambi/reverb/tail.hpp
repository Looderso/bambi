// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "bambi/dsp/biquad.hpp"
#include "bambi/reverb/tailplan.hpp"

/*  Reverb's tail: a feedback delay network.
 *
 *      in, a signal a line --> cuts, tonal correction, pre-delay --+
 *      scattered reflections, already late enough -----------------+--> diffuser: 4 x (delay, shuffle, Hadamard)
 *                                                                          |
 *          +-- Householder <-- shelves, decay, sub-bass blocker <-- allpass <-- line, read where it drifts
 *          +--> line  ...........  heard where it starts, and a third and two thirds along
 *                                                                          |
 *      out, a signal a line  <-- every output an orthogonal, sign-flipped mix of all of them <--+
 *
 *  It knows nothing of ambisonics: reading the bus at each line's direction and playing the lines back
 *  at them is the caller's, so the tail can run at fewer directions than the bus has channels.
 *
 *  Everything is allocated at prepare, for the longest plan there can be; configure() adopts a plan and
 *  set() the numbers it is played with, both with no allocation. Per sample throughout, so how the
 *  host cuts its blocks cannot matter.
 */
namespace bambi {

struct TailSettings {
    double rtLow{2.2}, rtMid{1.9}, rtHigh{1.4};  ///< seconds, below lowHz, between, and above highHz
    double lowHz{250.0}, highHz{4000.0};
    double lowCutHz{150.0}, highCutHz{12000.0};  ///< on the way in, 12 dB an octave
    double preDelaySeconds{0.02};                ///< what `in` waits; the scattered reflections do not
    double diffusion{0.6};  ///< 0 feeds the lines the input as it came, 1 only what the diffuser made of it
    bool drift{true};       ///< each line's length wanders, never by more than 4 cents of pitch
};

class FdnTail {
public:
    void prepare(double sampleRate);

    /// Adopt a layout. The lines' contents no longer mean anything, so they are cleared.
    void configure(const TailPlan& plan) noexcept;

    void set(const TailSettings& settings) noexcept;

    /// Silence, and the drift back at its first step: a render must repeat.
    void reset() noexcept;

    /// `in` and `scattered` are `lines()` interleaved signals, one a line; `scattered` may be null.
    /// `out` is the same shape, and may not be either input.
    void process(const float* in, const float* scattered, float* out, int frames) noexcept;

    int lines() const { return plan_.lines; }

    /// Energy summed over every line, for an impulse of unit energy into the lines at the mid decay:
    /// (1 + g^(2/3) + g^(4/3)) / 3 / (1 - g^2) a line, g its gain a pass, times (1-a)^2 + a^2 for a
    /// diffusion of a, since the input as it came and the input diffused are separate signals.
    double energyForUnitImpulse() const noexcept;

    /// A line's read distance as it stands, in samples: its length plus its drift. For tests.
    double delayOf(int line) const noexcept;

private:
    struct Ring {
        std::vector<float> data;
        int size{1}, write{0};
    };
    void newSegment(int line) noexcept;
    double random(int line) noexcept;

    double fs_{48000.0};
    TailPlan plan_;
    TailSettings settings_;
    int maxDrift_{480};

    std::array<Ring, kMaxTailLines> line_, allpass_, preDelay_;
    std::array<std::array<Ring, kMaxTailLines>, kDiffuserSteps> diffuser_;
    std::array<BiquadCoeffs, kMaxTailLines> shelfLow_, shelfHigh_;
    std::array<BiquadState, kMaxTailLines> stateLow_, stateHigh_, stateInLow_, stateInHigh_, stateCutLow_,
        stateCutHigh_;
    BiquadCoeffs correctLow_, correctHigh_, cutLow_, cutHigh_;
    std::array<double, kMaxTailLines> gainMid_{}, gainThird_{}, gainTwoThirds_{}, dcIn_{}, dcOut_{};
    std::array<double, kMaxTailLines> driftFrom_{}, driftTo_{}, driftAt_{}, driftStep_{};
    std::array<std::uint32_t, kMaxTailLines> rng_{};
    double dcCoeff_{0.0}, driftDepth_{0.0}, driftDepthNow_{0.0}, driftSlope_{0.0};
    int preDelaySamples_{0};
};

}  // namespace bambi
