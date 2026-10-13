// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

namespace bambi {

/*  A loudspeaker layout as it is loaded, typed or placed: the one description every decoder is designed from.
 *  Degrees and metres here, at the boundary; the design works in unit vectors. x is front, azimuth counter-clockwise
 *  from the front, elevation positive up. The channel is the output it plays on, counted from 1; the numbers may
 *  have gaps (a subwoofer's channel, which the decoder skips). An imaginary loudspeaker takes part in the panning
 *  and has no output.
 */
struct Loudspeaker {
    double azimuthDeg{0}, elevationDeg{0};
    double radiusM{1.0};
    int channel{1};
    bool imaginary{false};
    double gain{1.0};  ///< a trim the layout file carries; linear

    friend bool operator==(const Loudspeaker&, const Loudspeaker&) = default;
};

struct Layout {
    std::vector<Loudspeaker> speakers;
};

/// The widest bus a layout may use: outputs counted from its first channel, and the link's record of it.
inline constexpr int kMaxDecoderOutputs = 64;

/// The loudspeakers that are fed: every one that is not imaginary.
inline int realLoudspeakers(const std::vector<Loudspeaker>& layout) {
    int n = 0;
    for (const Loudspeaker& s : layout)
        if (!s.imaginary) ++n;
    return n;
}

}  // namespace bambi
