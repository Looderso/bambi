// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

#include "Playhead.h"
#include "bambi/live/graph.hpp"
#include "bambi/live/transport.hpp"

/*  Plays a compiled plan: the steps bambi-live's graph turned the host's shape into, run in order every block over
 *  the plugin instances the plan's slots name. The loop device's channels come in -- pair p is channels 2p and
 *  2p + 1 -- and the decoder's go out.
 *
 *  prepare() allocates everything a block will touch: a field buffer per node, return and Main; for each slot a
 *  buffer as wide as the plugin's widest side, its sidechain left silent; a delay line for every step that delays.
 *  process() allocates, frees and locks nothing, so it runs on the audio thread. A send's level is read from the
 *  runner's table at every block; set it on the audio thread, or hand it over to it.
 *
 *  The instances are the caller's, prepared by it at the plan's order and at least `maxBlock`; the runner only
 *  calls processBlock on them, and gives each its playhead. That playhead reads the projector set with setClock(),
 *  asked before every piece for where Live is at the piece's first sample, the runner counting the host's samples
 *  from prepare(). Without a projector the plugins see a transport that is stopped at 0.
 */
namespace bambi::livehost {

class Runner {
public:
    struct Setup {
        int order{3};
        int maxBlock{512};
        int loopChannels{16};
        int outputChannels{2};
        double rate{48000.0};
    };

    /// False, with `error` set, when the plan names a slot it was given no instance for.
    bool prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                 const Setup& setup, juce::String& error);

    /// Live's timeline, owned by the caller on the audio thread, which feeds it Live's transport. Set before audio
    /// runs; none: stopped.
    void setClock(live::TransportProjector* clock) noexcept { clock_ = clock; }

    /// The host's sample the next piece starts at.
    std::int64_t sample() const noexcept { return sample_; }

    /// A send's level, by the entry the compiled steps name: node * returns + return. Any thread: each entry is a
    /// value of its own, ordered against nothing, read at the next block.
    void setLevel(int entry, float level) noexcept {
        if (entry >= 0 && entry < levelCount_)
            levels_[static_cast<std::size_t>(entry)].store(level, std::memory_order_relaxed);
    }

    /// `frames` of the loop device in, the decoder's out, played in pieces of at most `maxBlock`. Output channels
    /// past the decoder's are silent.
    void process(const juce::AudioBuffer<float>& loop, juce::AudioBuffer<float>& output, int frames) noexcept;

private:
    struct Line {  // one delayed step's memory: `length` samples per channel, the oldest at `at`
        juce::AudioBuffer<float> memory;
        int length{0};
        int at{0};
    };

    void run(int slot, juce::AudioBuffer<float>& scratch) noexcept;
    void piece(const juce::AudioBuffer<float>& loop, int loopStart, juce::AudioBuffer<float>& output, int outStart,
               int frames) noexcept;

    live::Compiled compiled_;
    std::vector<juce::AudioProcessor*> slots_;
    Setup setup_;
    int field_{16};  // channels of the field at the plan's order
    std::vector<juce::AudioBuffer<float>> buffers_;
    std::vector<juce::AudioBuffer<float>> scratch_;  // per slot
    std::vector<Line> lines_;                        // per step
    std::unique_ptr<std::atomic<float>[]> levels_;
    int levelCount_{0};
    juce::MidiBuffer midi_;
    LivePlayhead playhead_;
    live::TransportProjector* clock_{nullptr};
    std::int64_t sample_{0};
};

}  // namespace bambi::livehost
