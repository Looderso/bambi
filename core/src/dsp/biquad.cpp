// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace bambi {
namespace {

constexpr double kPiD = 3.14159265358979323846;

double cornerOf(double hz, double sampleRate) { return 2.0 * kPiD * std::min(hz, 0.45 * sampleRate) / sampleRate; }

BiquadCoeffs normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

}  // namespace

BiquadCoeffs lowShelf(double hz, double gainDb, double sampleRate) {
    const double A = std::pow(10.0, gainDb / 40.0), w = cornerOf(hz, sampleRate), cs = std::cos(w);
    const double alpha = std::sin(w) / 2.0 * std::sqrt(2.0), sq = 2.0 * std::sqrt(A) * alpha;
    return normalised(A * ((A + 1) - (A - 1) * cs + sq), 2 * A * ((A - 1) - (A + 1) * cs),
                      A * ((A + 1) - (A - 1) * cs - sq), (A + 1) + (A - 1) * cs + sq, -2 * ((A - 1) + (A + 1) * cs),
                      (A + 1) + (A - 1) * cs - sq);
}

BiquadCoeffs highShelf(double hz, double gainDb, double sampleRate) {
    const double A = std::pow(10.0, gainDb / 40.0), w = cornerOf(hz, sampleRate), cs = std::cos(w);
    const double alpha = std::sin(w) / 2.0 * std::sqrt(2.0), sq = 2.0 * std::sqrt(A) * alpha;
    return normalised(A * ((A + 1) + (A - 1) * cs + sq), -2 * A * ((A - 1) + (A + 1) * cs),
                      A * ((A + 1) + (A - 1) * cs - sq), (A + 1) - (A - 1) * cs + sq, 2 * ((A - 1) - (A + 1) * cs),
                      (A + 1) - (A - 1) * cs - sq);
}

BiquadCoeffs lowPass(double hz, double q, double sampleRate) {
    const double w = cornerOf(hz, sampleRate), c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalised((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

BiquadCoeffs highPass(double hz, double q, double sampleRate) {
    const double w = cornerOf(hz, sampleRate), c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalised((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

double magnitudeAt(const BiquadCoeffs& k, double hz, double sampleRate) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPiD * hz / sampleRate), z2 = z1 * z1;
    return std::abs((k.b0 + k.b1 * z1 + k.b2 * z2) / (1.0 + k.a1 * z1 + k.a2 * z2));
}

}  // namespace bambi
