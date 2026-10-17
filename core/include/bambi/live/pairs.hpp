// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

/*  Which stereo pair of the loop device each Live track plays on. A track keeps its pair for as long as it
 *  exists, whatever else is added, removed or reordered around it: a source that changed pair mid-mix would
 *  jump to another encoder. A new track takes the lowest pair nobody holds.
 *
 *  A track is an opaque id the Live side chooses; keeping it stable is that side's business. Pair p is the
 *  loop device's channels 2p and 2p + 1, counted from 0. Runs on the host's message thread and allocates.
 */
namespace bambi::live {

using TrackId = std::uint64_t;

struct TrackPair {
    TrackId track{0};
    int pair{0};
    friend bool operator==(const TrackPair&, const TrackPair&) = default;
};

struct PairAssignment {
    std::vector<TrackPair> pairs;     ///< one per track that has a pair, in the order the tracks were given
    std::vector<TrackId> unassigned;  ///< tracks past the device's pairs, in the order given; never wrapped onto one
};

/// `tracks` as they are now, a repeated id counted once; `previous` as last assigned; `pairCount` the pairs the
/// loop device has. A previous pair beyond `pairCount`, or one already held, is given up and assigned afresh.
PairAssignment assignPairs(std::span<const TrackPair> previous, std::span<const TrackId> tracks, int pairCount);

}  // namespace bambi::live
