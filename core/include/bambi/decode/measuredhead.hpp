// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "bambi/decode/headmodel.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/vec3.hpp"

/*  A measured head -- the KU100, a SOFA file's -- as a HeadModel, so the decoder plays it through the
 *  one chain it has. The two ears' summed level and their difference become spherical-harmonic tables
 *  over the ERB axis, fitted with a penalty on roughness so a grid with a gap (no measurements below
 *  the listener) does not let the high orders run wild there; the time difference is measured from the
 *  two ears below 1.5 kHz and fitted the same way. No notch layer and no size morph: the head is what
 *  was measured, and the head slider scales only its time difference.
 *
 *  Units as the model's: dB, ERB numbers, microseconds left minus right. Directions are unit vectors,
 *  x front, y left, z up; responses are the two ears' impulse responses at one sample rate.
 */
namespace bambi {

struct MeasuredHead {
    double sampleRate{48000.0};
    int taps{0};
    std::vector<Vec3> directions;
    std::vector<float> left, right;  ///< directions x taps each
    std::string name;                ///< what the model says it was learned from
};

struct HeadFit {
    int order{20};            ///< the level tables'
    int itdOrder{10};         ///< the time difference's
    double roughness{1e-3};   ///< the penalty on order n is roughness * n (n + 1), relative to the fit's own scale
    double ildLimitDb{40.0};  ///< the far ear's notches held to this
    /// About 300 Hz to 20 kHz at 0.2 ERB: half the points, so half the file, of an axis at 0.1.
    HeadModel::Axis axis{7.7853, 0.2, 170};
};

/*  The measured time difference of one direction, microseconds, left minus right: the cross-correlation of the
 *  two ears weighted by a fourth-order low-pass at 1.5 kHz applied both ways, read at an eighth of a sample and
 *  refined between samples. prepare() allocates; microseconds() does not, so one estimator serves a whole set. */
class ItdEstimator {
public:
    bool prepare(double sampleRate);
    double microseconds(std::span<const float> left, std::span<const float> right);

private:
    double rate_{48000.0};
    RealFft fft_, up_;
    std::vector<double> l_, r_, lr_, li_, rr_, ri_, cr_, ci_, cc_;
};

/// Fits the model. False, with `error` set, when the set is empty, its arrays do not fit its counts, or the fit
/// cannot hold (fewer directions than the order's channels). Allocates; seconds for a dense grid at order 20.
bool fitHeadModel(const MeasuredHead& measured, const HeadFit& fit, HeadModel& out, std::string* error = nullptr);

/// How far a model at size 1 and no shift is from the set it was fitted to, over [loHz, hiHz) and every
/// direction: RMS in dB of the summed level and of the difference, RMS in microseconds of the time difference.
/// Measured on `axis` -- a finer one than the model's shows what its interpolation between points costs -- or on
/// the model's own when none is given.
struct HeadFitError {
    double cllDb{0}, ildDb{0}, itdUs{0};
};
HeadFitError headFitError(const MeasuredHead& measured, const HeadModel& model, double loHz, double hiHz,
                          const HeadModel::Axis* axis = nullptr);

}  // namespace bambi
