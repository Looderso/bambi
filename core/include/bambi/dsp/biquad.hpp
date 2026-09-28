// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/*  A second-order section, and the four designs Reverb's tail is made of: a low and a high shelf
 *  (Robert Bristow-Johnson's, at a slope of 1, which has no overshoot) and a low-pass and a high-pass
 *  at a given Q. Double throughout: these sit inside a feedback loop that runs for seconds.
 *
 *  Coefficients and state are kept apart, because a tail has one design shared by nothing and
 *  sixteen states -- and because a design is rebuilt in a control step while a state must survive it.
 */
namespace bambi {

struct BiquadCoeffs {
    double b0{1.0}, b1{0.0}, b2{0.0}, a1{0.0}, a2{0.0};  ///< y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
};

struct BiquadState {
    double x1{0.0}, x2{0.0}, y1{0.0}, y2{0.0};
    void clear() noexcept { x1 = x2 = y1 = y2 = 0.0; }
};

inline double run(const BiquadCoeffs& c, BiquadState& s, double x) noexcept {
    const double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

/// A shelf of `gainDb` below `hz` (low) or above it (high), and unity on the other side. The corner is
/// held under 0.45 of the sample rate.
BiquadCoeffs lowShelf(double hz, double gainDb, double sampleRate);
BiquadCoeffs highShelf(double hz, double gainDb, double sampleRate);

/// Twelve decibels an octave. A Q of 1/sqrt(2) is the flattest, 3 dB down at `hz`.
BiquadCoeffs lowPass(double hz, double q, double sampleRate);
BiquadCoeffs highPass(double hz, double q, double sampleRate);

/// The gain of a section at a frequency, as a magnitude: what a test or a drawing reads.
double magnitudeAt(const BiquadCoeffs& c, double hz, double sampleRate);

}  // namespace bambi
