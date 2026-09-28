// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "bambi/dsp/field.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"

namespace bambi {
namespace {

constexpr double kNoBlur = 0.25 * kDeg2Rad;
constexpr double kTiny = 1e-15;  // below this a band state is silence, and is made exactly zero

double onePole(double hz, double sampleRate) {
    return 1.0 - std::exp(-2.0 * kPi * std::min(hz, 0.45 * sampleRate) / sampleRate);
}

}  // namespace

void BandState::prepare(int order) {
    low.assign(static_cast<std::size_t>(numChannels(order)), 0.0);
    high.assign(low.size(), 0.0);
}

void BandState::clear() noexcept {
    std::fill(low.begin(), low.end(), 0.0);
    std::fill(high.begin(), high.end(), 0.0);
}

void PassWork::prepare(int order, int maxFrames) {
    order_ = order;
    maxFrames_ = maxFrames;
    warp_.prepare(order);
    rotation_.assign(static_cast<std::size_t>(rotationSize(order)), 0.0);
    warpBlocks_.assign(static_cast<std::size_t>(warpSize(order)), 0.0);
    caps_.assign(static_cast<std::size_t>(order + 1), 1.0);
    a_.assign(static_cast<std::size_t>(numChannels(order) * maxFrames), 0.0f);
    b_.assign(a_.size(), 0.0f);
}

void prepareTap(TapTransform& tap, int order) {
    tap.order = order;
    tap.toPole.assign(static_cast<std::size_t>(rotationSize(order)), 0.0f);
    tap.fromPole.assign(tap.toPole.size(), 0.0f);
    tap.warp.assign(static_cast<std::size_t>(warpSize(order)), 0.0f);
    tap.cosM.assign(static_cast<std::size_t>(order + 1), 1.0f);
    tap.sinM.assign(static_cast<std::size_t>(order + 1), 0.0f);
    tap.gains.assign(static_cast<std::size_t>(order + 1), 1.0f);
}

void buildTap(const TapSettings& given, double sampleRate, PassWork& work, TapTransform& tap) noexcept {
    const int N = tap.order;

    //  A NaN setting would circulate until the next reset; each takes its no-op value instead.
    TapSettings s = given;
    const auto number = [](double v, double otherwise) { return std::isfinite(v) ? v : otherwise; };
    if (!std::isfinite(s.axis.x) || !std::isfinite(s.axis.y) || !std::isfinite(s.axis.z)) {
        s.axis = {0.0, 0.0, 1.0};
        s.axisIsPole = true;
    }
    s.skew = number(s.skew, 0.0);
    s.spinRad = number(s.spinRad, 0.0);
    s.blurRad = number(s.blurRad, 0.0);
    s.lowCutHz = number(s.lowCutHz, 0.0);
    s.highCutHz = number(s.highCutHz, sampleRate);

    tap.plain = s.axisIsPole;
    if (!tap.plain) {
        shRotation(axisToPole(s.axis), N, work.rotation_);
        fieldBlocks(work.rotation_, N, tap.toPole, tap.fromPole);
    }

    tap.skewed = s.skew != 0.0;
    if (tap.skewed) {
        work.warp_.build(s.skew, work.warpBlocks_);
        axialBlocks(work.warpBlocks_, N, tap.warp);
    }

    for (int m = 0; m <= N; ++m) {
        tap.cosM[static_cast<std::size_t>(m)] = static_cast<float>(std::cos(m * s.spinRad));
        tap.sinM[static_cast<std::size_t>(m)] = static_cast<float>(std::sin(m * s.spinRad));
    }

    if (s.blurRad < kNoBlur) {
        std::fill(tap.gains.begin(), tap.gains.end(), 1.0f);
    } else {
        capWeights(s.blurRad, N, work.caps_);
        for (int n = 0; n <= N; ++n)
            tap.gains[static_cast<std::size_t>(n)] = static_cast<float>(work.caps_[static_cast<std::size_t>(n)]);
    }

    tap.lowpass = onePole(s.highCutHz, sampleRate);
    tap.highpass = onePole(s.lowCutHz, sampleRate);
}

void onePass(const TapTransform& tap, BandState& band, PassWork& work, float* data, int frames) noexcept {
    const int N = tap.order, channels = numChannels(N);
    const std::size_t bytes = static_cast<std::size_t>(channels * frames) * sizeof(float);

    float* cur = data;
    if (!tap.plain) {
        rotateField(tap.toPole, N, cur, work.a_.data(), frames);
        cur = work.a_.data();
    }
    if (tap.skewed) {
        float* next = cur == work.a_.data() ? work.b_.data() : work.a_.data();
        applyAxial(tap.warp, tap.cosM, tap.sinM, N, cur, next, frames);  // the skew, and the spin as it is put back
        cur = next;
    } else {
        spinField(tap.cosM, tap.sinM, N, cur, frames);
    }
    if (!tap.plain)
        rotateField(tap.fromPole, N, cur, data, frames);
    else if (cur != data)
        std::memcpy(data, cur, bytes);

    orderGains(tap.gains, N, data, frames);

    //  The band: a low-pass at the high cut, minus a low-pass of that at the low cut. Frame-outer
    //  order reads the interleaved field sequentially; channel-outer costs twice the whole pass.
    const double aLow = tap.lowpass, aHigh = tap.highpass;
    double* low = band.low.data();
    double* high = band.high.data();
    for (int f = 0; f < frames; ++f) {
        float* x = data + f * channels;
        for (int c = 0; c < channels; ++c) {
            double l = low[c] + aLow * (static_cast<double>(x[c]) - low[c]);
            double h = high[c] + aHigh * (l - high[c]);
            //  Flush to exact zero by compare, not a CPU flag, so every machine agrees. Written so
            //  that NaN is flushed too.
            l = !(std::abs(l) >= kTiny) ? 0.0 : l;
            h = !(std::abs(h) >= kTiny) ? 0.0 : h;
            low[c] = l;
            high[c] = h;
            x[c] = static_cast<float>(l - h);
        }
    }
}

}  // namespace bambi
