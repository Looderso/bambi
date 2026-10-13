// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <complex>
#include <span>
#include <vector>

#include "bambi/decode/headmodel.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/vec3.hpp"

/*  The decoder's binaural filters: one FIR per ambisonic channel and ear, designed from a Head.
 *  Magnitude least squares -- a complex least-squares fit up to the cutoff where the order can
 *  still hold the phase, above it a fit of the magnitudes alone with each bin's phase carried from
 *  the bin below -- then the diffuse-field covariance constraint, a 2x2 mix per bin that gives the
 *  decode the head's own diffuse-field ear spectra and interaural coherence.
 *
 *  prepare() allocates; build() does not and is meant for a worker that rebuilds the filters when a
 *  slider moves. The builder has one owner and is not re-entrant: build() rewrites its tables, its
 *  FFT scratch and the filters in place, so nothing reads filter(), head() or the errors while it
 *  runs, and a slider move during a build marks it stale rather than starting a second one. The
 *  audio thread never holds a span into the filters: whoever drives the builder copies a finished
 *  set -- every channel, both ears, from one build -- into a snapshot the audio thread adopts whole
 *  at a block boundary, convolves with an FFT of its own, and gets the first set built before the
 *  first block so that what is heard, and bounced, is a function of the parameters alone.
 */
namespace bambi {

struct FilterDesign {
    int order{1};
    double sampleRate{48000.0};
    int fftSize{512};  ///< a multiple of 32 made of 2s, 3s and 5s; the filters are designed on its bins
    int taps{256};     ///< the filters' length, at most fftSize
    int delay{64};     ///< samples: a bulk delay that keeps the part of a response ahead of the head's centre causal
    int fade{32};      ///< a half-Hann over the last taps
    double headRadius{0.0875};  ///< metres; the cutoff between the phase and the magnitude fit is order c / (2 pi r)
};

class BinauralFilterBuilder {
public:
    /// `directions` and `weights` (summing to 1) are the quadrature the fit runs over. Allocates. False when the
    /// design does not hold together (an FFT size pffft refuses, more taps than the FFT, an order out of range);
    /// build() then leaves the filters at zero.
    bool prepare(const HeadModel& model, const FilterDesign& design, std::span<const Vec3> directions,
                 std::span<const double> weights);

    /// Another head model on the same grid and design. Allocates the head's tables; never while build() may run,
    /// so on the worker, between builds.
    void setModel(const HeadModel& model);
    /// Synthesises the head for the two sliders and designs every filter. Allocates nothing.
    void build(double earShiftErb, double headSize);

    int channels() const { return channels_; }
    int taps() const { return design_.taps; }
    double cutoffHz() const { return cutoff_; }
    const Head& head() const { return head_; }
    std::span<const float> filter(int acn, int ear) const;  ///< ear 0 is the left; taps() samples

    /// Mean over the bins in [loHz, hiHz) of the direction-weighted RMS magnitude error of the decoded ears
    /// against the synthesised head, dB. Reads the last build(). A diagnostic: it allocates.
    double magnitudeError(double loHz, double hiHz) const;
    /// The largest difference over [loHz, hiHz) between the decode's diffuse-field ear spectrum and the head's, dB.
    double diffuseError(double loHz, double hiHz) const;

private:
    using Cx = std::complex<double>;
    int bins() const { return design_.fftSize / 2 + 1; }
    double binHz(int k) const { return k * design_.sampleRate / design_.fftSize; }
    void decodedEars(int k, std::span<Cx> out) const;  // the decode of bin k at every direction, directions x 2

    FilterDesign design_;
    bool ready_{false};
    Head head_;
    RealFft fft_;
    int directions_{0}, channels_{0};
    double cutoff_{0};
    std::vector<Vec3> grid_;         // directions, kept for setModel
    std::vector<double> binErbs_;    // bins: each bin's frequency as an ERB number
    std::vector<double> weight_;     // directions
    std::vector<double> basis_;      // directions x channels: Y
    std::vector<double> pinv_;       // channels x directions: the weighted pseudo-inverse of Y
    std::vector<double> diffuse_;    // channels: a diffuse field's power per channel
    std::vector<double> window_;     // taps
    std::vector<double> amplitude_;  // bins x directions x 2: the head's magnitudes, linear
    std::vector<Cx> target_;         // directions x 2: one bin's HRTFs
    std::vector<Cx> carried_;        // directions x 2: the previous bin's decode at every direction
    std::vector<Cx> decode_;         // bins x channels x 2: the designed spectra
    std::vector<double> re_, im_, ir_;
    std::vector<float> filters_;  // channels x 2 x taps
};

}  // namespace bambi
