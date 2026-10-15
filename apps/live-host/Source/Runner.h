// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

#include "Playhead.h"
#include "bambi/link/handoff.hpp"
#include "bambi/live/graph.hpp"
#include "bambi/live/transport.hpp"

/*  Plays a compiled plan: the steps bambi-live's graph turned the host's shape into, run in order every block over
 *  the plugin instances the plan's slots name. The loop device's channels come in -- pair p is channels 2p and
 *  2p + 1 -- and the decoder's go out.
 *
 *  A plan and everything it plays with is one program: the steps, the instances, a field buffer per node, return
 *  and Main, for each slot a buffer as wide as the plugin's widest side, its sidechain left silent, a delay line for
 *  every step that delays, the send levels. It is built whole on the message thread, by prepare() before audio
 *  runs and by swap() while it does. A swapped program is taken at a block: the output fades out over kFade
 *  samples, the new program takes over, and it fades back in. The program it replaced, and every instance only it
 *  kept, is freed on the message thread -- by the next swap(), or by tidy(). process() allocates, frees and locks
 *  nothing, so it runs on the audio thread.
 *
 *  The instances are the caller's, prepared by it at the plan's order and at least `maxBlock`; what a program must
 *  keep alive is handed to it as `keep`. The runner only calls processBlock on them, and gives each its playhead.
 *  That playhead reads the projector set with setClock(), asked before every piece for where Live is at the
 *  piece's first sample, the runner counting the host's samples from prepare(). Without a projector the plugins see
 *  a transport that is stopped at 0.
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
    using Keep = std::vector<std::shared_ptr<void>>;  ///< what a program keeps alive while it may be played

    static constexpr int kFade = 256;  ///< samples of the fade out before a swap, and of the fade in after

    /// The first program, before any audio. False, with `error` set, when the plan names a slot it was given no
    /// instance for, or a buffer or pair it does not have.
    bool prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                 const Setup& setup, juce::String& error, Keep keep = {});

    /// A new program, while audio runs, at the setup prepare() was given; the same checks.
    bool swap(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
              juce::String& error, Keep keep = {});

    /// Frees what the audio thread has let go of. Message thread.
    void tidy() { handoff_.freeRetired(); }

    /// Whether a swapped program is still waiting for the audio thread.
    bool swapping() const noexcept { return handoff_.pending(); }

    /// Live's timeline, owned by the caller on the audio thread, which feeds it Live's transport. Set before audio
    /// runs; none: stopped.
    void setClock(live::TransportProjector* clock) noexcept { clock_ = clock; }

    /// The host's sample the next piece starts at.
    std::int64_t sample() const noexcept { return sample_; }

    /// A send's level, by the entry the compiled steps name: node * returns + return, in the program last
    /// prepared or swapped. Message thread: each entry is a value of its own, ordered against nothing, read at the
    /// next block.
    void setLevel(int entry, float level) noexcept;

    /// `frames` of the loop device in, the decoder's out, played in pieces of at most `maxBlock`. Output channels
    /// past the decoder's are silent.
    void process(const juce::AudioBuffer<float>& loop, juce::AudioBuffer<float>& output, int frames) noexcept;

private:
    struct Line {  // one delayed step's memory: `length` samples per channel, the oldest at `at`
        juce::AudioBuffer<float> memory;
        int length{0};
        int at{0};
    };

    //  Never changed once posted but for its working memory, which only the audio thread touches.
    struct Program : SnapshotCensus {
        live::Compiled compiled;
        std::vector<juce::AudioProcessor*> slots;
        Keep keep;
        mutable std::vector<juce::AudioBuffer<float>> buffers;
        mutable std::vector<juce::AudioBuffer<float>> scratch;  // per slot
        mutable std::vector<Line> lines;                        // per step
        std::unique_ptr<std::atomic<float>[]> levels;
        int levelCount{0};
    };

    std::unique_ptr<Program> build(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots,
                                   int levels, juce::String& error, Keep keep);
    void run(const Program& p, int slot, juce::AudioBuffer<float>& scratch) noexcept;
    void piece(const Program& p, const juce::AudioBuffer<float>& loop, int loopStart, juce::AudioBuffer<float>& output,
               int outStart, int frames) noexcept;

    Setup setup_;
    int field_{16};  // channels of the field at the plan's order
    Handoff<Program> handoff_;
    Program* latest_{nullptr};  // the program last prepared or swapped: the message thread's
    juce::MidiBuffer midi_;
    LivePlayhead playhead_;
    live::TransportProjector* clock_{nullptr};
    std::int64_t sample_{0};
    enum class Fade { None, Out, In };
    Fade fade_{Fade::None};  // the audio thread's, with `faded_`: samples of the fade played
    int faded_{0};
};

}  // namespace bambi::livehost
