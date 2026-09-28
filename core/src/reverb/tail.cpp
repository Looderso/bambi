// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/tail.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {
namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr double kAllpassGain = 0.5;
constexpr double kDriftCents = 4.0, kDriftSwingSeconds = 0.002, kDriftSegment[2] = {0.3, 0.9};
constexpr double kMaxPreDelay = 0.25;
constexpr double kMostGain = 0.99995;

void hadamard(float* f, int K) {
    for (int h = 1; h < K; h *= 2)
        for (int i = 0; i < K; i += 2 * h)
            for (int j = i; j < i + h; ++j) {
                const float a = f[j], b = f[j + h];
                f[j] = a + b;
                f[j + h] = a - b;
            }
    const float k = 1.0f / std::sqrt(static_cast<float>(K));
    for (int i = 0; i < K; ++i) f[i] *= k;
}

}  // namespace

void FdnTail::prepare(double sampleRate) {
    fs_ = sampleRate;
    const TailLimits lim = tailLimits(sampleRate);
    maxDrift_ = static_cast<int>(std::ceil(0.01 * sampleRate));
    for (int k = 0; k < kMaxTailLines; ++k) {
        const auto i = static_cast<std::size_t>(k);
        line_[i].data.assign(static_cast<std::size_t>(lim.line + maxDrift_ + 8), 0.0f);
        allpass_[i].data.assign(static_cast<std::size_t>(lim.allpass), 0.0f);
        preDelay_[i].data.assign(static_cast<std::size_t>(std::ceil(kMaxPreDelay * sampleRate)) + 2, 0.0f);
        for (auto& step : diffuser_) step[i].data.assign(static_cast<std::size_t>(lim.diffuser + 1), 0.0f);
    }
    configure(planTail(16, sampleRate, 1.0, 0.06));
    set(TailSettings{});
}

void FdnTail::configure(const TailPlan& plan) noexcept {
    plan_ = plan;
    for (int k = 0; k < plan_.lines; ++k) {
        const auto i = static_cast<std::size_t>(k);
        line_[i].size = std::min(static_cast<int>(line_[i].data.size()), plan_.length[i] + maxDrift_ + 8);
        allpass_[i].size = std::min(static_cast<int>(allpass_[i].data.size()), std::max(1, plan_.allpass[i]));
        preDelay_[i].size = static_cast<int>(preDelay_[i].data.size());
        for (int s = 0; s < kDiffuserSteps; ++s) {
            Ring& r = diffuser_[static_cast<std::size_t>(s)][i];
            r.size = std::min(static_cast<int>(r.data.size()), plan_.diffuserDelay[static_cast<std::size_t>(s)][i] + 1);
        }
    }
    reset();
    set(settings_);
}

void FdnTail::reset() noexcept {
    for (int k = 0; k < kMaxTailLines; ++k) {
        const auto i = static_cast<std::size_t>(k);
        for (Ring* r : {&line_[i], &allpass_[i], &preDelay_[i]}) {
            std::fill(r->data.begin(), r->data.end(), 0.0f);
            r->write = 0;
        }
        for (auto& step : diffuser_) {
            std::fill(step[i].data.begin(), step[i].data.end(), 0.0f);
            step[i].write = 0;
        }
        for (BiquadState* s :
             {&stateLow_[i], &stateHigh_[i], &stateInLow_[i], &stateInHigh_[i], &stateCutLow_[i], &stateCutHigh_[i]})
            s->clear();
        dcIn_[i] = dcOut_[i] = 0.0;
        //  The drift starts again from the same place, so a render repeats.
        rng_[i] = static_cast<std::uint32_t>(static_cast<std::uint32_t>(k + 1) * 2654435761u);
        if (rng_[i] == 0) rng_[i] = 1;
        driftFrom_[i] = driftTo_[i] = 0.0;
        driftAt_[i] = 1.0;
        driftStep_[i] = 0.0;
    }
    driftDepthNow_ = driftDepth_;
}

void FdnTail::set(const TailSettings& s) noexcept {
    settings_ = s;
    const double rt[3] = {std::max(0.05, s.rtLow), std::max(0.05, s.rtMid), std::max(0.05, s.rtHigh)};
    for (int k = 0; k < plan_.lines; ++k) {
        const auto i = static_cast<std::size_t>(k);
        const double trip = plan_.length[i] + plan_.allpass[i];  // a trip round the line includes its allpass
        const double gLow = std::min(kMostGain, std::pow(10.0, -3.0 * trip / (rt[0] * fs_)));
        const double gMid = std::min(kMostGain, std::pow(10.0, -3.0 * trip / (rt[1] * fs_)));
        const double gHigh = std::min(kMostGain, std::pow(10.0, -3.0 * trip / (rt[2] * fs_)));
        gainMid_[i] = gMid;
        gainThird_[i] = std::pow(gMid, 1.0 / 3.0);
        gainTwoThirds_[i] = std::pow(gMid, 2.0 / 3.0);
        //  Around the mid gain, so every band loses exactly what its decay asks a pass.
        shelfLow_[i] = lowShelf(s.lowHz, 20.0 * std::log10(gLow / gMid), fs_);
        shelfHigh_[i] = highShelf(s.highHz, 20.0 * std::log10(gHigh / gMid), fs_);
    }
    //  Jot's correction: a band's energy grows with its decay, so its level goes with the root of mid
    //  over band -- decay then changes how long a band rings, not the tail's tone.
    correctLow_ = lowShelf(s.lowHz, 10.0 * std::log10(rt[1] / rt[0]), fs_);
    correctHigh_ = highShelf(s.highHz, 10.0 * std::log10(rt[1] / rt[2]), fs_);
    cutLow_ = highPass(s.lowCutHz, std::sqrt(0.5), fs_);
    cutHigh_ = lowPass(s.highCutHz, std::sqrt(0.5), fs_);
    preDelaySamples_ = std::clamp(static_cast<int>(std::lround(s.preDelaySeconds * fs_)), 0,
                                  static_cast<int>(preDelay_[0].data.size()) - 2);
    driftDepth_ = s.drift ? std::min(kDriftSwingSeconds * fs_, static_cast<double>(maxDrift_)) : 0.0;
    driftSlope_ = std::pow(2.0, kDriftCents / 1200.0) - 1.0;  // the most a delay may change a sample
    dcCoeff_ = std::exp(-2.0 * kPiD * 25.0 / fs_);
}

double FdnTail::random(int line) noexcept {
    std::uint32_t x = rng_[static_cast<std::size_t>(line)];
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_[static_cast<std::size_t>(line)] = x;
    return static_cast<double>(x) / 4294967296.0;
}

/*  A new stretch of drift for a line: from where the last ended, and only as far as the pitch limit
 *  allows in its time. A smoothstep over T seconds changes the delay by at most 1.5 |b - a| depth /
 *  (T fs) samples a sample, and a delay changing by d a sample shifts pitch by 1200 log2(1 + d) cents. */
void FdnTail::newSegment(int line) noexcept {
    const auto i = static_cast<std::size_t>(line);
    const double a = driftTo_[i];
    const double T = kDriftSegment[0] + random(line) * (kDriftSegment[1] - kDriftSegment[0]);
    const double step = std::min(2.0, driftSlope_ * T * fs_ / (1.5 * std::max(driftDepth_, 1e-6)));
    double b = a + (2.0 * random(line) - 1.0) * step;
    if (b > 1.0) b = 2.0 - b;
    if (b < -1.0) b = -2.0 - b;
    driftFrom_[i] = a;
    driftTo_[i] = b;
    driftStep_[i] = 1.0 / (T * fs_);
}

double FdnTail::delayOf(int line) const noexcept {
    const auto i = static_cast<std::size_t>(line);
    const double t = driftAt_[i], at = driftFrom_[i] + (driftTo_[i] - driftFrom_[i]) * t * t * (3.0 - 2.0 * t);
    return plan_.length[i] + driftDepthNow_ * at;
}

double FdnTail::energyForUnitImpulse() const noexcept {
    double sum = 0.0;
    for (int k = 0; k < plan_.lines; ++k) {
        const double g = gainMid_[static_cast<std::size_t>(k)];
        sum += (1.0 + std::pow(g, 2.0 / 3.0) + std::pow(g, 4.0 / 3.0)) / 3.0 / (1.0 - g * g);
    }
    //  What reaches the lines is part of the input as it came and part of it diffused -- two different
    //  signals, so their energies add, and at a diffusion of a half only half of what went in goes round.
    const double a = std::clamp(settings_.diffusion, 0.0, 1.0);
    return sum / plan_.lines * ((1.0 - a) * (1.0 - a) + a * a);
}

void FdnTail::process(const float* in, const float* scattered, float* out, int frames) noexcept {
    const int K = plan_.lines;
    const auto a = static_cast<float>(std::clamp(settings_.diffusion, 0.0, 1.0));
    std::array<float, kMaxTailLines> raw{}, u{}, tmp{}, o{}, wv{}, tA{}, tB{}, mix{};

    for (int f = 0; f < frames; ++f) {
        // ---- in: pre-delay, the cuts, the tonal correction; then the diffuser ---------------------
        for (int k = 0; k < K; ++k) {
            const auto i = static_cast<std::size_t>(k);
            Ring& pre = preDelay_[i];
            pre.data[static_cast<std::size_t>(pre.write)] = in[f * K + k];
            double x = pre.data[static_cast<std::size_t>((pre.write - preDelaySamples_ + pre.size) % pre.size)];
            pre.write = (pre.write + 1) % pre.size;
            if (scattered != nullptr) x += scattered[f * K + k];
            x = run(cutHigh_, stateCutHigh_[i], run(cutLow_, stateCutLow_[i], x));
            x = run(correctHigh_, stateInHigh_[i], run(correctLow_, stateInLow_[i], x));
            raw[i] = u[i] = static_cast<float>(x);
        }
        for (int s = 0; s < kDiffuserSteps; ++s) {
            const auto si = static_cast<std::size_t>(s);
            for (int k = 0; k < K; ++k) {
                Ring& r = diffuser_[si][static_cast<std::size_t>(k)];
                tmp[static_cast<std::size_t>(k)] = r.data[static_cast<std::size_t>(r.write)];
                r.data[static_cast<std::size_t>(r.write)] = u[static_cast<std::size_t>(k)];
                r.write = (r.write + 1) % r.size;
            }
            for (int k = 0; k < K; ++k)
                u[static_cast<std::size_t>(k)] =
                    tmp[static_cast<std::size_t>(plan_.diffuserFrom[si][static_cast<std::size_t>(k)])] *
                    plan_.diffuserSign[si][static_cast<std::size_t>(k)];
            hadamard(u.data(), K);
        }

        // ---- the loop: a drifting cubic read, the allpass, absorption a band, the sub-bass blocker --
        double sum = 0.0;
        driftDepthNow_ +=
            (driftDepth_ - driftDepthNow_) * 2e-5;  // on or off glides over seconds, with no jump of pitch
        for (int k = 0; k < K; ++k) {
            const auto i = static_cast<std::size_t>(k);
            Ring& L = line_[i];
            double t = driftAt_[i] + driftStep_[i];
            if (t >= 1.0) {
                newSegment(k);
                t = 0.0;
            }
            driftAt_[i] = t;
            const double at = driftFrom_[i] + (driftTo_[i] - driftFrom_[i]) * t * t * (3.0 - 2.0 * t);
            const double delay = plan_.length[i] + driftDepthNow_ * at;
            double r = L.write - delay;
            while (r < 2.0) r += L.size;
            const int r0 = static_cast<int>(std::floor(r));
            const double frac = r - r0;
            const auto sample = [&](int n) {
                return static_cast<double>(L.data[static_cast<std::size_t>(n % L.size)]);
            };
            const double xm1 = sample(r0 - 1), x0 = sample(r0), x1 = sample(r0 + 1), x2 = sample(r0 + 2);
            const double c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2,
                         c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
            const double y0 = ((c3 * frac + c2) * frac + c1) * frac + x0;

            //  Lossless, so the decay is untouched, but it smears each round trip: without it the energy
            //  came back in pulses, one a trip.
            Ring& A = allpass_[i];
            const double delayed = A.data[static_cast<std::size_t>(A.write)], fed = y0 - kAllpassGain * delayed;
            A.data[static_cast<std::size_t>(A.write)] = static_cast<float>(fed);
            A.write = (A.write + 1) % A.size;
            const double y = delayed + kAllpassGain * fed;

            double v = gainMid_[i] * run(shelfHigh_[i], stateHigh_[i], run(shelfLow_[i], stateLow_[i], y));
            const double blocked = v - dcIn_[i] + dcCoeff_ * dcOut_[i];
            dcIn_[i] = v;
            dcOut_[i] = std::abs(blocked) < 1e-20 ? 0.0 : blocked;  // silence, exactly, by a compare
            v = dcOut_[i];
            o[i] = static_cast<float>(v);
            sum += v;
        }

        // ---- Householder back into the lines, with the diffused input -- part of it as it came ------
        const auto reflect = static_cast<float>(2.0 * sum / K);
        for (int k = 0; k < K; ++k) {
            const auto i = static_cast<std::size_t>(k);
            Ring& L = line_[i];
            const float w = (o[i] - reflect) + (1.0f - a) * raw[i] + a * u[i];
            //  Heard where the line starts, and a third and two thirds along at the decay it has there: a
            //  line returns only after its whole length, and with one tap the tail fell into a gap first.
            const int third = static_cast<int>(std::lround(plan_.length[i] / 3.0)),
                      twoThirds = static_cast<int>(std::lround(2.0 * plan_.length[i] / 3.0));
            tA[i] = static_cast<float>(gainThird_[i]) *
                    L.data[static_cast<std::size_t>((L.write - third + L.size) % L.size)];
            tB[i] = static_cast<float>(gainTwoThirds_[i]) *
                    L.data[static_cast<std::size_t>((L.write - twoThirds + L.size) % L.size)];
            L.data[static_cast<std::size_t>(L.write)] = w;
            wv[i] = w;
            L.write = (L.write + 1) % L.size;
        }

        // ---- out: every output hears an orthogonal, sign-flipped mix of every line -----------------
        const float third = 1.0f / std::sqrt(3.0f);
        const int jA = K >> 3, jB = (5 * K) >> 3;
        for (int j = 0; j < K; ++j) {
            const auto& P = plan_.outFrom;
            const auto& S = plan_.outSign;
            mix[static_cast<std::size_t>(j)] =
                third * (wv[static_cast<std::size_t>(P[static_cast<std::size_t>(j)])] * S[static_cast<std::size_t>(j)] +
                         tA[static_cast<std::size_t>(P[static_cast<std::size_t>((j + jA) % K)])] *
                             S[static_cast<std::size_t>((j + jB) % K)] +
                         tB[static_cast<std::size_t>(P[static_cast<std::size_t>((j + jB) % K)])] *
                             S[static_cast<std::size_t>((j + jA) % K)]);
        }
        hadamard(mix.data(), K);
        for (int j = 0; j < K; ++j) out[f * K + j] = mix[static_cast<std::size_t>(j)];
    }
}

}  // namespace bambi
