// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/scene/energy.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
constexpr double kInputShare = 0.1;  ///< the input takes the colour where it is within 10 dB of what the plugin added
constexpr double kSmoothingSeconds = 0.110;
}  // namespace

// ---- the audio thread's half -------------------------------------------------------------

void CovarianceWindow::prepare(int order, int framesPerWindow) {
    order_ = std::max(0, order);
    channels_ = numChannels(order_);
    framesPerWindow_ = std::max(1, framesPerWindow);
    sums_.assign(static_cast<std::size_t>(covarianceSize(order_)), 0.0);
    filled_ = 0;
}

void CovarianceWindow::reset() noexcept {
    std::fill(sums_.begin(), sums_.end(), 0.0);
    filled_ = 0;
}

void CovarianceWindow::add(const float* interleaved, int frames) noexcept {
    if (interleaved == nullptr || frames <= 0 || channels_ <= 0) return;
    for (int f = 0; f < frames; ++f) {
        const float* x = interleaved + static_cast<std::size_t>(f) * static_cast<std::size_t>(channels_);
        std::size_t at = 0;
        for (int i = 0; i < channels_; ++i) {
            const double xi = static_cast<double>(x[i]);
            for (int j = i; j < channels_; ++j) sums_[at++] += xi * static_cast<double>(x[j]);
        }
    }
    filled_ += frames;
}

void CovarianceWindow::take(std::span<float> into) noexcept {
    const double scale = filled_ > 0 ? 1.0 / static_cast<double>(filled_) : 0.0;
    const auto n = std::min(into.size(), sums_.size());
    for (std::size_t i = 0; i < n; ++i) into[i] = static_cast<float>(sums_[i] * scale);
    reset();
}

// ---- the picture -------------------------------------------------------------------------

Vec3 EnergyField::directionOf(int texel) {
    const int t = std::clamp(texel, 0, kEnergyTexels - 1);
    const int ix = t % kEnergyWidth;
    const int iy = t / kEnergyWidth;
    /*  The same way round as the equirect view draws it, both ways: row 0 is the top, which is up, and
        column 0 is the left edge, which is the back reached by way of the left -- azimuth +pi,
        falling to -pi at the right edge, because azimuth is counter-clockwise and left is positive
        (`project`'s -azimuth / pi). */
    const double az = kPi - (static_cast<double>(ix) + 0.5) / kEnergyWidth * 2.0 * kPi;
    const double el = kPi / 2.0 - (static_cast<double>(iy) + 0.5) / kEnergyHeight * kPi;
    return fromAzEl(az, el);
}

double EnergyField::columnOf(double azimuthRad) { return (kPi - azimuthRad) / (2.0 * kPi) * kEnergyWidth - 0.5; }

double EnergyField::energyAt(int texel, bool added) const {
    const auto t = static_cast<std::size_t>(std::clamp(texel, 0, kEnergyTexels - 1));
    const auto& from = added ? rawWet_ : rawDry_;
    return t < from.size() ? static_cast<double>(from[t]) : 0.0;
}

void EnergyField::prepare(int order) {
    order_ = std::max(0, order);
    channels_ = numChannels(order_);

    std::vector<double> weights(static_cast<std::size_t>(order_ + 1), 1.0);
    maxRE(order_, weights);

    //  the beam a max-rE decoder forms: each order's weight times (2n+1), which SN3D leaves out
    for (int n = 0; n <= order_; ++n) weights[static_cast<std::size_t>(n)] *= 2 * n + 1;
    //  a unit plane wave seen by the beam pointing at it has amplitude sum wn (2n+1), and that is full scale
    double peakAmplitude = 0.0;
    for (int n = 0; n <= order_; ++n) peakAmplitude += weights[static_cast<std::size_t>(n)];
    fullScale_ = peakAmplitude * peakAmplitude;

    beams_.assign(static_cast<std::size_t>(kEnergyTexels) * static_cast<std::size_t>(channels_), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(channels_), 0.0);
    for (int t = 0; t < kEnergyTexels; ++t) {
        shSN3D(directionOf(t), order_, y);
        float* row = beams_.data() + static_cast<std::size_t>(t) * static_cast<std::size_t>(channels_);
        for (int c = 0; c < channels_; ++c)
            row[c] =
                static_cast<float>(y[static_cast<std::size_t>(c)] * weights[static_cast<std::size_t>(acnOrder(c))]);
    }

    rawWet_.assign(kEnergyTexels, 0.0f);
    rawDry_.assign(kEnergyTexels, 0.0f);
    wetE_.assign(kEnergyTexels, 0.0);
    dryE_.assign(kEnergyTexels, 0.0);
    shown_.assign(kEnergyTexels, 0.0f);
    share_.assign(kEnergyTexels, 0.0f);
    clear();
}

void EnergyField::clear() noexcept {
    std::fill(rawWet_.begin(), rawWet_.end(), 0.0f);
    std::fill(rawDry_.begin(), rawDry_.end(), 0.0f);
    std::fill(wetE_.begin(), wetE_.end(), 0.0);
    std::fill(dryE_.begin(), dryE_.end(), 0.0);
    std::fill(shown_.begin(), shown_.end(), 0.0f);
    std::fill(share_.begin(), share_.end(), 0.0f);
    since_ = kEnergyHoldSeconds + 1.0;
}

/*  b^T C b for both covariances, with C packed as its upper triangle: the off-diagonal terms count
 *  twice, because the lower half is the same numbers and was never sent.
 *
 *  One pass over the beam table, not two. The table is the big thing here -- 8,192 rows of
 *  `channels` floats -- and reading it once for both answers is what makes an order-7 picture
 *  affordable at all.
 */
void EnergyField::beamEnergies(std::span<const float> wet, std::span<const float> dry) {
    std::fill(rawWet_.begin(), rawWet_.end(), 0.0f);
    std::fill(rawDry_.begin(), rawDry_.end(), 0.0f);
    const auto want = static_cast<std::size_t>(covarianceSize(order_));
    const float* w = wet.size() >= want ? wet.data() : nullptr;
    const float* d = dry.size() >= want ? dry.data() : nullptr;
    if (channels_ <= 0 || (w == nullptr && d == nullptr)) return;

    for (int t = 0; t < kEnergyTexels; ++t) {
        const float* b = beams_.data() + static_cast<std::size_t>(t) * static_cast<std::size_t>(channels_);
        float sumW = 0.0f, sumD = 0.0f;
        std::size_t at = 0;
        for (int i = 0; i < channels_; ++i) {
            const float bi = b[i];
            const float bii = bi * bi;
            if (w != nullptr) sumW += bii * w[at];
            if (d != nullptr) sumD += bii * d[at];
            ++at;
            for (int j = i + 1; j < channels_; ++j, ++at) {
                const float bij = 2.0f * bi * b[j];
                if (w != nullptr) sumW += bij * w[at];
                if (d != nullptr) sumD += bij * d[at];
            }
        }
        rawWet_[static_cast<std::size_t>(t)] = std::max(0.0f, sumW);
        rawDry_[static_cast<std::size_t>(t)] = std::max(0.0f, sumD);
    }
}

void EnergyField::sample(std::span<const float> wet, std::span<const float> dry) {
    if (channels_ <= 0) return;
    beamEnergies(wet, dry);
    since_ = 0.0;
}

float EnergyField::alphaOf(double energy) const noexcept {
    constexpr double darkEnergy = 6.309573444801933e-5;  // 10^(kEnergyDarkDb / 10)
    constexpr double fullEnergy = 0.5011872336272722;    // 10^(kEnergyFullDb / 10)
    static_assert(kEnergyDarkDb == -42.0 && kEnergyFullDb == -3.0, "the two energies above follow these");
    const double e = energy / fullScale_;
    if (!(e > darkEnergy)) return 0.0f;  // under the dark point, and not a number, draw nothing
    if (e >= fullEnergy) return 1.0f;
    const double t = (10.0 * std::log10(e) - kEnergyDarkDb) / (kEnergyFullDb - kEnergyDarkDb);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));  // smooth at both ends: no edge where it starts to show
}

/*  Every frame, on what the beams last measured. The one-pole runs on the energy and not on the
    alpha, because smoothing a logarithm smooths the wrong quantity. Each texel is then drawn from its
    own energy only. */
bool EnergyField::step(double dt) {
    if (channels_ <= 0) return false;
    since_ += std::max(0.0, dt);
    if (since_ > kEnergyHoldSeconds) {
        //  Painting stops 1.6 s after the last covariance: a picture of nothing is not a picture.
        std::fill(shown_.begin(), shown_.end(), 0.0f);
        return false;
    }

    const double k = dt <= 0.0 ? 1.0 : 1.0 - std::exp(-std::max(dt, 1e-3) / kSmoothingSeconds);
    for (int t = 0; t < kEnergyTexels; ++t) {
        const auto i = static_cast<std::size_t>(t);
        wetE_[i] += (static_cast<double>(rawWet_[i]) - wetE_[i]) * k;
        dryE_[i] += (static_cast<double>(rawDry_[i]) - dryE_[i]) * k;
        shown_[i] = alphaOf(wetE_[i] + dryE_[i]);
        share_[i] = dryE_[i] >= wetE_[i] * kInputShare ? 0.0f : 1.0f;
    }
    return true;
}

}  // namespace bambi
