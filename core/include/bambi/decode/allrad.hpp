// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <string>
#include <vector>

#include "bambi/math/sphere.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/patch/layout.hpp"

/*  AllRAD: the loudspeaker decoder for a layout, designed from its positions alone. A dense grid of
 *  directions is panned to the loudspeakers by VBAP over the convex hull of their directions, and the
 *  decoder is that panning sampled against the spherical harmonics. Imaginary loudspeakers take part
 *  in the hull and are dropped from the output -- never mixed back, which would cost a third of the
 *  order. A layout with nothing below the listener gets a point at the nadir, one with nothing above a
 *  point at the zenith; a ring with nothing above or below is designed on the ring alone, panning
 *  between neighbours in azimuth.
 *
 *  Allocates and runs on a worker, never the audio thread. Doubles and ordered containers throughout,
 *  and the loudspeakers are sorted by channel first, so the same layout gives the same decoder in any
 *  listing order and on any machine.
 */
namespace bambi {

/// The listening situation: one listener at the centre decodes unweighted, an audience spread across the room
/// max-rE, which widens the sweet area and costs the centred listener sharpness and treble.
enum class Weighting { Basic, MaxRe };

struct SpeakerDesign {
    int order{0};
    int outputs{0};              ///< rows of the matrix: the highest real loudspeaker's channel
    std::vector<double> matrix;  ///< outputs x (order + 1)^2, row-major, for AmbiX (ACN, SN3D) in; a channel without a
                                 ///< loudspeaker, or with an imaginary one, has a row of zeros
    std::vector<int> channel;    ///< per real loudspeaker, by channel: its output, counted from 0
    std::vector<double> gain;    ///< per real loudspeaker: r / r_max, so the farthest plays at 1
    std::vector<double> delaySeconds;           ///< per real loudspeaker: (r_max - r) / c, so the farthest has none
    bool ring{false};                           ///< designed on the ring: every point within 10 degrees of the horizon
    std::vector<Vec3> points;                   ///< every point the panning used, real and imaginary, unit vectors
    std::vector<bool> imaginary;                ///< per point
    std::vector<std::array<int, 3>> triangles;  ///< the hull's faces as point indices (3D only)
};

/// Designs the decoder; `order` 1..7. False, with `error` set, when the layout does not surround the listener
/// (the origin outside the hull), repeats a channel, or holds no real loudspeaker.
bool designAllrad(const Layout& layout, int order, Weighting weighting, SpeakerDesign& out,
                  std::string* error = nullptr);

}  // namespace bambi
