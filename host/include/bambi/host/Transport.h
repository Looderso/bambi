// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>

/*  Where the host says we are, and whether it jumped.
 *
 *  Every plugin in the suite resets its engine on a transport start and on a locate, and every one
 *  must decide "is this a locate?" the same way -- determinism is defined against the host's
 *  timeline, and two plugins that disagreed about where a timeline jumped would put two different
 *  renders in the same bounce. So this is one piece of code rather than one per plugin.
 */
namespace bambi::host {

struct TransportReading {
    bool playing{false};
    std::int64_t timeline{-1};  ///< samples; -1 when the host does not say
    double ppq{0.0};
    double bpm{120.0};
    /// Start again from `start`: playback began, or the timeline jumped. The caller decides what that means for its engine -- what is reset and what carries on is the plugin's own.
    bool restart{false};
    /// The timeline sample a restart starts from. `timeline` when the host says where it is; 0 when it does not, so a play still starts everything from zero.
    std::int64_t start{0};
};

class TransportWatch {
public:
    /// Preparing to play: nothing is expected, so the next block that plays is a start.
    void forget() noexcept {
        wasPlaying_ = false;
        expected_ = -1;
    }

    /*  Read the head and say whether this block starts again. Real-time safe. `slackSamples` is how
     *  far the host's reported timeline may sit from where the last block expected it before that
     *  counts as a locate rather than imprecision -- one block's worth plus a little, from the
     *  caller.
     *
     *  Why a tolerance: CLAP carries no sample position in its transport, only seconds and beats, so
     *  a wrapper must derive timeInSamples and lands a sample either side of exact. Worse, some
     *  wrappers split a host buffer into sub-blocks at events and report the host block's transport
     *  for every one, so the timeline stands still across sub-blocks and then jumps a whole block at
     *  the next buffer -- which a plain `!=` reads as a reset several times a block. */
    TransportReading observe(juce::AudioPlayHead* head, int numSamples, std::int64_t slackSamples,
                             double sampleRate) noexcept {
        TransportReading r;
        if (head != nullptr) {
            if (const auto position = head->getPosition()) {
                r.playing = position->getIsPlaying();
                if (const auto t = position->getTimeInSamples())
                    r.timeline = *t;
                else if (const auto seconds = position->getTimeInSeconds(); seconds && *seconds >= 0.0)
                    r.timeline = static_cast<std::int64_t>(std::llround(*seconds * sampleRate));
                if (const auto p = position->getPpqPosition()) r.ppq = *p;
                if (const auto b = position->getBpm()) r.bpm = *b;
            }
        }

        //  a host that says nowhere at all still says when it plays: each play starts from zero
        const bool known = r.timeline >= 0;
        const auto drift = r.timeline - expected_;
        const bool jumped = expected_ < 0 || drift > slackSamples || drift < -slackSamples;
        r.restart = r.playing && (!wasPlaying_ || (known && jumped));
        r.start = known ? r.timeline : 0;

        wasPlaying_ = r.playing;
        expected_ = (r.playing && r.timeline >= 0) ? r.timeline + numSamples : -1;
        return r;
    }

private:
    bool wasPlaying_{false};
    std::int64_t expected_{-1};
};

}  // namespace bambi::host
