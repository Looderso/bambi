// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "bambi/live/protocol.hpp"

/*  Where each track's session clip is, between Live's reports: the last report a track gave, placed on the host's
 *  samples, run on at the tempo, looping in its loop. A track whose report says no clip plays has none.
 *
 *  A fixed number of tracks, so nothing allocates: the audio thread owns one and feeds it the reports as they
 *  arrive. A track past the capacity is not followed.
 */
namespace bambi::live {

class ClipClock {
public:
    static constexpr std::size_t kTracks = 128;

    /// Live's report, as it holds from host sample `at`.
    void receive(const SlotPlay& report, std::int64_t at) noexcept;

    /// The slot a track plays, or -1.
    int playing(TrackId track) const noexcept;
    /// Whether the session has the track rather than the arrangement: a clip plays on it, or one did since Back to
    /// Arrangement.
    bool overridden(TrackId track) const noexcept;

    /// Where in its clip `track` is at host sample `sample`, when it plays `slot`; none else.
    std::optional<double> position(TrackId track, int slot, std::int64_t sample, double bpm,
                                   double sampleRate) const noexcept;

private:
    struct Entry {
        TrackId track{0};
        SlotPlay report;
        std::int64_t at{0};
    };
    const Entry* find(TrackId track) const noexcept;
    std::array<Entry, kTracks> entries_{};
    std::size_t used_{0};
};

}  // namespace bambi::live
