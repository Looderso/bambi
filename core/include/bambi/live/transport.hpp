// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

/*  Live's timeline, carried to the host. Live's transport arrives as messages -- playing, song position in
 *  quarter notes, tempo -- a few milliseconds late and unevenly; the host's engines want a playhead they
 *  can read at every block, as a plugin reads its host's. This turns the first into the second.
 *
 *  Everything is in the host's own samples. A message comes with the host sample at which Live was where
 *  the message says -- its arrival, moved by the measured latency, is the caller's arithmetic -- so a
 *  reading is a function of the messages and the sample asked about, and not of when or how often it is
 *  asked.
 *
 *  What a reading promises the engines downstream, which take a jump in `timeline` for a locate and reset:
 *    - `timeline` advances by exactly one per sample while playing, and jumps only at a start or when
 *      Live's position is further from the projection than `slackSamples` -- a real locate;
 *    - a message within the slack never moves `timeline` and never makes `ppq` go back: the difference is
 *      closed by running slightly fast or slow, by at most `maxSkew`, so a clock that drifts against the
 *      host's is followed without a locate;
 *    - a tempo change takes effect from the message's sample on;
 *    - a message never changes a reading already given: one whose sample was read already takes effect from
 *      the last sample read, carried forward at its tempo.
 *
 *  `timeline` at a start or a locate is the song position at the current tempo, in samples -- exact
 *  without tempo changes before it, which Live's position in beats cannot say more about. Not real-time
 *  safe to share: one owner calls receive() and reading() in turn, the audio thread or behind its handoff.
 */
namespace bambi::live {

struct TransportMessage {
    bool playing{false};
    double ppq{0.0};    ///< Live's song position, quarter notes
    double bpm{120.0};  ///< > 0
};

struct Playhead {
    bool playing{false};
    std::int64_t timeline{0};  ///< samples
    double ppq{0.0};
    double bpm{120.0};
};

struct ProjectorSettings {
    double sampleRate{48000.0};
    std::int64_t slackSamples{2400};     ///< further than this from the projection is a locate
    std::int64_t catchUpSamples{24000};  ///< a difference inside the slack is closed over this many samples
    double maxSkew{0.01};                ///< the most the projection runs fast or slow, as a fraction of the tempo
};

class TransportProjector {
public:
    explicit TransportProjector(ProjectorSettings settings = {}) : s_(settings) {}

    /// Live was where `m` says at host sample `at`. Messages arrive with `at` increasing.
    void receive(const TransportMessage& m, std::int64_t at) noexcept;

    /// Where Live is at host sample `sample`, asked with `sample` increasing. Before any message: stopped at 0.
    Playhead reading(std::int64_t sample) noexcept;

private:
    double nominal(double bpm) const noexcept { return bpm / (60.0 * s_.sampleRate); }  // quarter notes per sample
    double ppqAt(std::int64_t sample) const noexcept;
    std::int64_t timelineAt(std::int64_t sample) const noexcept;
    std::int64_t samplesOf(double ppq, double bpm) const noexcept;
    void anchor(std::int64_t at, double ppq, std::int64_t timeline, double perSample) noexcept;

    ProjectorSettings s_;
    bool known_{false};
    bool playing_{false};
    double bpm_{120.0};
    std::int64_t sample_{0};                                       // the anchor: a host sample,
    double ppq_{0.0};                                              // where Live was there,
    std::int64_t timeline_{0};                                     // the timeline there,
    double perSample_{0.0};                                        // and how fast the projection runs from it
    std::int64_t read_{std::numeric_limits<std::int64_t>::min()};  // the latest sample read
};

}  // namespace bambi::live
