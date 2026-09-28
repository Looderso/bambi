// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/encode/encoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

void Encoder::prepare(int order) {
    order_ = std::clamp(order, 0, kMaxOrder);
    const auto n = static_cast<std::size_t>(numChannels());
    current_.assign(n, 0.0);
    start_.assign(n, 0.0);
    target_.assign(n, 0.0);
    y_.assign(n, 0.0);
    yB_.assign(n, 0.0);
    capW_.assign(static_cast<std::size_t>(order_ + 1), 1.0);
    setTarget({1, 0, 0}, 0.0);
    snap();
}

void Encoder::setTarget(Vec3 direction, double widthRad, int rampSamples, double linearGain) {
    start_ = current_;
    rampLen_ = std::max(0, rampSamples);
    rampPos_ = 0;
    capWeights(widthRad, order_, capW_);
    shSN3D(unit(direction), order_, y_);
    //  By order, then by the channels within it: every channel of order n shares one weight, so
    //  nothing is looked up per channel -- a per-channel search is free at 16 channels and not at 1296.
    for (int n = 0; n <= order_; ++n) {
        const double w = capW_[static_cast<std::size_t>(n)] * linearGain;
        for (int c = n * n; c < (n + 1) * (n + 1); ++c)
            target_[static_cast<std::size_t>(c)] = y_[static_cast<std::size_t>(c)] * w;
    }
}

void Encoder::setTargetPair(Vec3 a, double weightA, Vec3 b, double weightB, double widthRad, int rampSamples,
                            double linearGain) {
    start_ = current_;
    rampLen_ = std::max(0, rampSamples);
    rampPos_ = 0;
    if (weightA == 0.0 && weightB == 0.0) {
        //  aimed at nothing, which is every step of `sum`: no harmonics are worked out to be multiplied by 0
        std::fill(target_.begin(), target_.end(), 0.0);
        return;
    }
    capWeights(widthRad, order_, capW_);
    shSN3D(unit(a), order_, y_);
    shSN3D(unit(b), order_, yB_);
    for (int n = 0; n <= order_; ++n) {
        const double w = capW_[static_cast<std::size_t>(n)] * linearGain;
        for (int c = n * n; c < (n + 1) * (n + 1); ++c) {
            const auto i = static_cast<std::size_t>(c);
            target_[i] = (y_[i] * weightA + yB_[i] * weightB) * w;
        }
    }
}

bool Encoder::silent() const {
    const auto zero = [](double g) { return g == 0.0; };
    return std::all_of(current_.begin(), current_.end(), zero) && std::all_of(target_.begin(), target_.end(), zero);
}

StereoPair stereoPairAbout(Vec3 direction, double spreadRad) {
    const Vec3 d = unit(direction);
    const double level = std::sqrt(d.x * d.x + d.y * d.y);  // cos(elevation)
    if (level < 1e-9) return {d, d};                        // at the pole there is no left: the pair is the point
    const Vec3 left{-d.y / level, d.x / level, 0.0};
    const double open = std::min(1.0, level / std::sin(10.0 * kDeg2Rad));  // closes over the last ten degrees
    const double half = 0.5 * spreadRad * open;
    return {unit(d * std::cos(half) + left * std::sin(half)), unit(d * std::cos(half) - left * std::sin(half))};
}

double encodeOverlap(Vec3 a, Vec3 b, double widthRad, int order) {
    /*  SN3D's addition theorem: within an order, the sum over its channels of Y(a) Y(b) is P_n(cos).
        A sound field's energy weights order n by (2n + 1) -- that is what N3D is -- so the overlap
        that keeps two points at one point's level carries it in; a plain dot product of the SN3D
        gains leaves it out and reads up to 1.35 dB low at order 3. */
    const int n = std::clamp(order, 0, kMaxOrder);
    //  on the stack: this runs in the control step, on the audio thread, where nothing allocates
    std::array<double, kMaxOrder + 1> weights{};
    const std::span<double> w(weights.data(), static_cast<std::size_t>(n + 1));
    capWeights(widthRad, n, w);
    const double x = std::clamp(dot(unit(a), unit(b)), -1.0, 1.0);
    double p0 = 1.0, p1 = x, both = 0.0, one = 0.0;
    for (int k = 0; k <= n; ++k) {
        const double pk = k == 0 ? p0 : (k == 1 ? p1 : ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k);
        if (k >= 2) {
            p0 = p1;
            p1 = pk;
        }
        const double ww = (2.0 * k + 1.0) * w[static_cast<std::size_t>(k)] * w[static_cast<std::size_t>(k)];
        both += ww * pk;
        one += ww;
    }
    return one > 0.0 ? both / one : 1.0;
}

void Encoder::snap() {
    current_ = target_;
    start_ = target_;
    rampLen_ = 0;
    rampPos_ = 0;
}

void Encoder::process(const float* in, std::span<float* const> out, int numSamples) {
    if (numSamples <= 0) return;
    const std::size_t nch = std::min(current_.size(), out.size());

    /*  Two ramp regimes. With an explicit ramp length the move takes the same wall-clock time
     *  however the host chops the buffer; without one it spans this call, for a target set once
     *  per buffer.
     *
     *  The timed ramp is indexed from an absolute sample position since setTarget, never
     *  accumulated across calls: accumulating is algebraically identical but numerically is not,
     *  since the rounding then depends on where the buffer boundaries fall, and the same input at
     *  block 64 and block 1024 would produce different bits. */
    const bool timed = rampLen_ > 0;

    for (std::size_t c = 0; c < nch; ++c) {
        float* dst = out[c];
        if (dst == nullptr) continue;

        if (timed) {
            const double a = start_[c], b = target_[c];
            if (a == b) {
                if (a == 0.0) continue;
                const auto g = static_cast<float>(a);
                for (int i = 0; i < numSamples; ++i) dst[i] += in[i] * g;
            } else {
                const double inv = 1.0 / static_cast<double>(rampLen_);
                for (int i = 0; i < numSamples; ++i) {
                    const int j = rampPos_ + i;
                    const double t = j >= rampLen_ ? 1.0 : static_cast<double>(j) * inv;
                    dst[i] += in[i] * static_cast<float>(a + (b - a) * t);
                }
            }
            const int end = std::min(rampPos_ + numSamples, rampLen_);
            current_[c] = a + (b - a) * (static_cast<double>(end) / rampLen_);
        } else {
            const float g0 = static_cast<float>(current_[c]);
            const float dg = static_cast<float>((target_[c] - current_[c]) / numSamples);
            if (dg == 0.0f) {
                if (g0 == 0.0f) continue;  // silent channel: touch nothing
                for (int i = 0; i < numSamples; ++i) dst[i] += in[i] * g0;
            } else {
                //  Indexed from i, never accumulated: an accumulator is a loop-carried
                //  dependency that blocks vectorisation, and it drifts.
                for (int i = 0; i < numSamples; ++i) dst[i] += in[i] * (g0 + dg * static_cast<float>(i));
            }
        }
    }

    if (timed)
        rampPos_ = std::min(rampPos_ + numSamples, rampLen_);
    else
        current_ = target_;
}

}  // namespace bambi
