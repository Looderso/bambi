// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

#include "bambi/math/vec3.hpp"

/*  Energy by direction, the visualiser every plugin shares: where the energy in the field is, and
 *  which of it this plugin added (two covariances, the input's and the added field's). The audio
 *  thread only accumulates covariances; the picture is built on the message thread, reading each
 *  texel through a max-rE beam as a decoder would.
 */
namespace bambi {

/// The texture: equirectangular, and the same one every projection resamples.
inline constexpr int kEnergyWidth = 128;
inline constexpr int kEnergyHeight = 64;
inline constexpr int kEnergyTexels = kEnergyWidth * kEnergyHeight;

/// How long after the last covariance the picture stops being painted at all.
inline constexpr double kEnergyHoldSeconds = 1.6;

/// Numbers in the upper triangle of an order's covariance, the diagonal included.
constexpr int covarianceSize(int order) {
    const int c = (order + 1) * (order + 1);
    return c * (c + 1) / 2;
}

/// Where (i, j), i <= j, sits in the packed upper triangle.
constexpr int covarianceIndex(int channels, int i, int j) { return i * channels - (i * (i - 1)) / 2 + (j - i); }

/*  Audio thread. Accumulates the upper triangle of one field's covariance over a window. Real-time
 *  safe after prepare(). Double, because a float sum over thousands of squares loses the quiet
 *  directions.
 */
class CovarianceWindow {
public:
    /// Allocates for this order and this many frames a window. Nothing after this allocates.
    void prepare(int order, int framesPerWindow);

    /// Forget the part-finished window: a locate, a reset, a stop.
    void reset() noexcept;

    /// `frames` interleaved frames. Real-time safe.
    void add(const float* interleaved, int frames) noexcept;

    /// A window is full and waiting to be taken.
    bool ready() const noexcept { return filled_ >= framesPerWindow_ && framesPerWindow_ > 0; }

    /// Copy the window out and start the next. A span shorter than covarianceSize(order) takes what fits.
    void take(std::span<float> into) noexcept;

    int order() const noexcept { return order_; }
    int channels() const noexcept { return channels_; }

private:
    std::vector<double> sums_;
    int order_{0}, channels_{0}, framesPerWindow_{0}, filled_{0};
};

/*  Message thread. Beam energies, three ballistics, and the colour:
 *
 *    level  a reference that jumps to a new peak and falls at 10 dB/s, so a decaying tail dims.
 *    shape  the field's mean energy is the zero of the scale, so a field with no direction left
 *           disappears (a point source loses only 0.3 dB of its peak at order 3).
 *    time   a 110 ms one-pole per texel, so the picture settles instead of flickering.
 */
class EnergyField {
public:
    /// Builds the beam table for this order. Allocates; call it off the audio thread.
    void prepare(int order);

    /// Everything the picture holds goes back: a reset, a stop, a change of order.
    void clear() noexcept;

    /// The expensive half, run when a covariance lands. `wet` is what this plugin added, `dry` what
    /// arrived; an empty span reads as no energy.
    void sample(std::span<const float> wet, std::span<const float> dry);

    /// The cheap half, every frame. False once the picture should stop being drawn.
    bool step(double dt);

    /// 0 where nothing is drawn, 1 at the reference. One per texel, equirectangular, row 0 at the top.
    std::span<const float> alpha() const { return shown_; }
    /// Per texel, 1 where only what this plugin added is drawn and 0 where the input is. Never between.
    std::span<const float> addedOnly() const { return share_; }

    /// The direction a texel looks at.
    static Vec3 directionOf(int texel);
    /// The fractional column an azimuth falls in: the inverse of directionOf.
    static double columnOf(double azimuthRad);

    /// A texel's beam energy b^T C b before the ballistics. For tests.
    double energyAt(int texel, bool added) const;

    int order() const noexcept { return order_; }
    /// The reference the scale is hung from, in the energy the beams measure. For tests.
    double reference() const noexcept { return reference_; }

private:
    /// Both covariances in one pass: the beam table (8192 x 64 floats at order 7) is read once.
    void beamEnergies(std::span<const float> wet, std::span<const float> dry);

    std::vector<float> beams_;            ///< kEnergyTexels x channels, the max-rE weighted basis
    std::vector<float> rawWet_, rawDry_;  ///< what the beams measured, before any smoothing
    std::vector<double> wetE_, dryE_;     ///< after smoothing, which runs on energy
    std::vector<float> shown_, share_;
    int order_{0}, channels_{0};
    double reference_{0.0};
    double since_{kEnergyHoldSeconds + 1.0};  ///< nothing has landed yet
};

}  // namespace bambi
