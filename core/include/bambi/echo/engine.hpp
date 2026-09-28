// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>

#include "bambi/dsp/fielddelay.hpp"
#include "bambi/dsp/ramp.hpp"
#include "bambi/echo/pass.hpp"
#include "bambi/region/projection.hpp"

/*  Echo's engine: four feedback loops fed by one input line.
 *
 *      in --> send --> input line --(read `offset` ago)--> ring <-- + feedback * y
 *                                                            | x = written one period ago
 *                                                            v
 *                                                        one pass --> y --> out += level * y
 *
 *  Repeat j lands at offset + j * period, at level * feedback^(j-1), having had the pass j times.
 *  Every change a frame brings is ramped over one hop from an absolute sample position, so the
 *  output does not depend on how the host cuts its blocks.
 */
namespace bambi {

inline constexpr int kEchoTaps = 4;
inline constexpr int kEchoHop = 256;   ///< samples between control steps; a frame is ramped over one
inline constexpr int kEchoChunk = 64;  ///< the most frames processed at once

struct EchoTapFrame {
    bool on{false};
    int periodSamples{18000};
    int offsetSamples{0};
    double level{1.0};     ///< linear, outside the loop
    double feedback{0.5};  ///< linear, inside it
    TapSettings pass;
};

struct EchoFrame {
    std::array<EchoTapFrame, kEchoTaps> taps;
    Region send;  ///< the part of the input that reaches the line, for all four taps
    double sendAmount{1.0};
    //  The output stage, after the loops: neither gain reaches a ring. Linear.
    double dryGain{0.0};
    double wetGain{1.0};
};

class EchoEngine {
public:
    /// Allocates everything, sized for the order's longest tap. Nothing after this allocates.
    void prepare(int order, double sampleRate);

    /// A play start, locate or loop: all state is cleared and the next frame applies without a ramp.
    void reset() noexcept;

    /// What the next hop ramps to. Real-time safe.
    void setFrame(const EchoFrame& frame) noexcept;

    /// Interleaved frames in and out; `in` and `out` may be the same buffer.
    void process(const float* in, float* out, int frames) noexcept;

    int order() const { return order_; }
    int channels() const { return channels_; }
    int capacityFrames() const { return capacity_; }
    bool asleep(int tap) const { return !taps_[static_cast<std::size_t>(tap)].awake; }
    /// Samples the ring-write guard has clamped at +80 dBFS since prepare.
    long long clamped() const { return clamped_; }

private:
    struct Tap {
        FieldDelayLine ring;
        TapTransform transform;
        BandState band;
        bool awake{false}, on{false};
        int period{18000}, previousPeriod{18000}, offset{0};
        Ramp<double> level, feedback;
    };

    int order_{0}, channels_{1}, capacity_{1};
    double sampleRate_{48000.0};
    FieldDelayLine line_;
    std::array<Tap, kEchoTaps> taps_;
    PassWork work_;
    RegionOperator send_;
    Region sendNow_;
    bool sendSet_{false};
    Ramp<double> sendAmount_{1.0, 1.0}, dry_{0.0, 0.0}, wet_{1.0, 1.0};
    std::vector<float> x_, older_, fed_, clean_, sent_;
    int sinceFrame_{kEchoHop};  ///< samples since setFrame, up to one hop
    bool snap_{true};
    long long clamped_{0};
};

}  // namespace bambi
