// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/pairs.hpp"

#include <algorithm>

namespace bambi::live {

PairAssignment assignPairs(std::span<const TrackPair> previous, std::span<const TrackId> tracks, int pairCount) {
    const auto count = static_cast<std::size_t>(std::max(pairCount, 0));
    std::vector<bool> held(count, false);
    std::vector<int> pairOf(tracks.size(), -1);
    std::vector<bool> repeat(tracks.size(), false);

    for (std::size_t i = 0; i < tracks.size(); ++i)
        repeat[i] = std::find(tracks.begin(), tracks.begin() + static_cast<std::ptrdiff_t>(i), tracks[i]) !=
                    tracks.begin() + static_cast<std::ptrdiff_t>(i);

    //  survivors first, so a new track can never take a pair that is still someone's
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (repeat[i]) continue;
        const auto was =
            std::find_if(previous.begin(), previous.end(), [&](const TrackPair& p) { return p.track == tracks[i]; });
        if (was == previous.end() || was->pair < 0 || static_cast<std::size_t>(was->pair) >= count ||
            held[static_cast<std::size_t>(was->pair)])
            continue;
        held[static_cast<std::size_t>(was->pair)] = true;
        pairOf[i] = was->pair;
    }

    PairAssignment out;
    std::size_t free = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (repeat[i]) continue;
        if (pairOf[i] < 0) {
            while (free < count && held[free]) ++free;
            if (free == count) {
                out.unassigned.push_back(tracks[i]);
                continue;
            }
            held[free] = true;
            pairOf[i] = static_cast<int>(free);
        }
        out.pairs.push_back({tracks[i], pairOf[i]});
    }
    return out;
}

}  // namespace bambi::live
