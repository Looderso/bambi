// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <string>
#include <vector>

#include "bambi/decode/allrad.hpp"
#include "bambi/decode/binaural.hpp"
#include "bambi/math/vec3.hpp"

/*  Filling a loudspeaker layout's gaps binaurally. What the layout covers plays as its own decoder plays it, dry;
 *  what it does not cover goes through the HRTF and on to the same loudspeakers through crosstalk cancellation,
 *  which gives each ear its own channel. For a plane wave from a loudspeaker's own direction, the HRTF followed by
 *  the cancellation is that loudspeaker's signal alone, so the dry and the binaural parts meet without a seam.
 *
 *  The two parts split the ambisonic signal by direction: a dry window and an HRTF window, each a matrix on the
 *  ambisonic channels. The layout's decoder plays the first, an order-3 binaural decode the second, and the
 *  cancellation takes the two ear signals to every real loudspeaker at once -- a 2 x 2 inversion per frequency,
 *  however many loudspeakers there are and wherever they stand.
 *
 *  Everything here designs and allocates; it runs on a worker, never on the audio thread.
 */
namespace bambi {

struct FillSettings {
    double holdDeg{20.0};            ///< the layout's own decode alone, dry, within this angle of what it covers
    double fadeDeg{60.0};            ///< then a raised-cosine crossfade to binaural over this angle
    bool equalPower{true};           ///< the dry and binaural gains' squares sum to one; false: the gains do
    double regularisationDb{-25.0};  ///< of the cancellation's inversion, against the head's mean response
    double bandLoHz{300.0};          ///< the cancellation fades in over [lo / 2, lo]
    double bandHiHz{4500.0};  ///< and out over [hi, 1.4 hi]; outside the band the ears' channels play uncancelled,
                              ///< each on the loudspeaker nearest 30 degrees to its side

    friend bool operator==(const FillSettings&, const FillSettings&) = default;
};

/// The binaural part's order, whatever the field's.
inline constexpr int kFillOrder = 3;
/// What the cancellation must reach for a layout to be filled -- the share of each ear's signal that the other ear
/// hears, mean over the band, dB -- with the inversion exact and regularised at kFillReferenceDb, so the verdict is the
/// layout's, whatever depth plays.
inline constexpr double kFillSeparationNeededDb = -25.0;
inline constexpr double kFillReferenceDb = -30.0;

/// The cancellation's delay at `sampleRate`, samples: 128 at 48 kHz, in time at other rates.
int fillCancellationDelay(double sampleRate);

/// The binaural design of the fill's HRTF part at `sampleRate`: order 3, and a bulk delay of 32 samples at 48 kHz
/// (in time at other rates), the shortest that keeps the filters' response.
FilterDesign fillFilterDesign(double sampleRate);

/*  What a layout covers, from its design: the spherical triangles of its hull whose corners are real loudspeakers
 *  (or centres of real coplanar groups), and the arcs that bound it -- the triangles' outer edges, a ring's arcs
 *  between real neighbours, a pair's arc. A single loudspeaker covers its own direction. */
struct CoveredRegion {
    std::vector<std::array<Vec3, 3>> triangles;  ///< wound outward: dot(cross(b - a, c - a), a) > 0
    std::vector<std::array<Vec3, 2>> arcs;       ///< the shorter great-circle arc between the two
    std::vector<Vec3> points;                    ///< loudspeakers on no arc
    bool sphere{false};                          ///< nothing left uncovered: the fill has nothing to do
};
CoveredRegion coveredRegion(const SpeakerDesign& design);

/// The angle from a unit vector to the nearest covered direction, radians; 0 inside.
double angleToRegion(const CoveredRegion& region, Vec3 direction);

/*  The two windows as matrices on the ambisonic channels, projected on a quadrature exact far past order 7: `dry`
 *  (order + 1)^2 square, applied before the layout's decoder; `hrtf` with (kFillOrder + 1)^2 rows and
 *  (order + 1)^2 columns, giving the binaural part its order-3 field. Row-major, SN3D, a' = W a. */
struct FillWindows {
    int order{0};
    std::vector<double> dry;
    std::vector<double> hrtf;
};
void fillWindows(const CoveredRegion& region, const FillSettings& settings, int order, FillWindows& out);

/// The layout's design with its decoder after the dry window, ready for the speaker renderer.
SpeakerDesign windowedDecoder(const SpeakerDesign& design, const FillWindows& windows);

/*  The crosstalk cancellation: from the two ear signals of the fill's binaural decode to each real loudspeaker,
 *  delayed by 128 samples at 48 kHz and 384 taps long (both in time at other rates). `filters` holds 2 ears x
 *  loudspeakers x taps, one filter's taps contiguous, so that MatrixConvolver::publish takes it with the ears as
 *  inputs and the loudspeakers as outputs.
 *
 *  Designed for loudspeakers that arrive together and alike at the listener, its feeds then carry each
 *  loudspeaker's distance gain and delay as the speaker renderer gives the dry part, so they add to the renderer's
 *  output; `taps` grows by the longest of those delays. The dry part waits for the binaural path -- `delay` plus
 *  the latency of each of the two convolvers in series. */
struct Cancellation {
    int taps{0};
    int delay{0};              ///< samples
    std::vector<int> outputs;  ///< per loudspeaker row: its output channel, counted from 0
    std::vector<float> filters;
    double separationDb{0.0};  ///< mean over the band: the share of each ear's channel the other ear hears, with
                               ///< the head the filters were designed for
    double level{1.0};  ///< the gain in the feeds that plays a loudspeaker's own direction as loud as the layout's
                        ///< decoder plays it, averaged over the loudspeakers
};

/// Designs it from the real loudspeakers of `design` and the fill's binaural filters in `head` (built from
/// fillFilterDesign). False, with `error` set, when fewer than two loudspeakers play or the separation falls short
/// of kFillSeparationNeededDb; `out` then holds what was designed, to say by how much.
bool designCancellation(const SpeakerDesign& design, const BinauralFilterBuilder& head, const FillSettings& settings,
                        double sampleRate, Cancellation& out, std::string* error = nullptr);

/// The gain on the whole -- the layout's decoder after the dry window, and the fill through `cancel` after the HRTF
/// window -- that plays every direction, on average, as loud as the layout's decoder alone, so switching the fill
/// changes the sound and not the loudness.
double fillLoudness(const SpeakerDesign& design, const FillWindows& windows, const Cancellation& cancel,
                    const BinauralFilterBuilder& head, double sampleRate);

}  // namespace bambi
