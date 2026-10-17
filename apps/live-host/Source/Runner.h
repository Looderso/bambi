// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <optional>
#include <vector>

#include "Playhead.h"
#include "bambi/link/handoff.hpp"
#include "bambi/live/graph.hpp"
#include "bambi/live/transport.hpp"

/*  Plays a compiled plan: the steps bambi-live's graph turned the host's shape into, run in order every block over
 *  the plugin instances the plan's slots name. The input row comes in -- the loop device's channels, Live's Main on
 *  the first two and a track's pair after them, then the chosen devices' inputs -- and the decoders' go out along the routes a program is given, each onto an output channel of its own,
 *  late by its delay, beside Live's Main as it came in. A program given no routes at all -- a check's -- plays the
 *  first decoder straight out; one given an empty list plays nothing.
 *
 *  A plan and everything it plays with is one program: the steps, the instances, a field buffer per node, return
 *  and Main, for each slot a buffer as wide as the plugin's widest side -- an Encoder's or an insert's sidechain
 *  hearing the pair its step names, or silence -- a delay line for
 *  every step that delays, the send levels. It is built whole on the message thread, by prepare() before audio
 *  runs and by swap() while it does. A swapped program is taken at a block: what the loop device brings and the
 *  output both fade out over kFade samples, silence is held through the old program's longest delay line, the new
 *  program takes over, and both fade back in -- so no insert or line kept across the swap hears a step. The program it replaced, and every instance only it
 *  kept, is freed on the message thread -- by the next swap(), or by tidy(). process() allocates, frees and locks
 *  nothing, so it runs on the audio thread.
 *
 *  Live's Main is played through delay lines of the runner's own, which a swap does not drop: it fades with the
 *  rest only when the program swapped in plays it with other delays or onto other channels, so a change to the
 *  field that keeps its latency is not heard on Main.
 *
 *  A send's level changes only on the host's grid of kLevelGrid samples: at each grid point the level last set is
 *  taken, and the send ramps to it linearly over the next kLevelGrid samples, so a moving knob does not click and
 *  what is heard does not depend on the block size. A program starts at its levels, with no ramp: the first at
 *  its first block, one swapped in where it takes over.
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
        int inputChannels{16};  ///< the input row: the loop device's channels, then the devices' inputs
        int outputChannels{2};
        double rate{48000.0};
        int mainInput{-1};       ///< the input row's channel Live's Main's left arrives on, its right after; -1 none
        int maxMainDelay{0};     ///< samples Main may be delayed by
    };
    /// A channel played out after the decoders: one of a decoder's, or of Live's Main, onto an output channel.
    struct Route {
        bool main{false};  ///< Live's Main: `from` 0 its left, 1 its right; otherwise a decoder's channel, from 0
        int decoder{0};    ///< which decoder, by its place among the plan's
        int from{0};
        int to{0};  ///< the output channel, from 0
        int delay{0};
        friend bool operator==(const Route&, const Route&) = default;
    };
    using Keep = std::vector<std::shared_ptr<void>>;  ///< what a program keeps alive while it may be played

    static constexpr int kFade = 256;       ///< samples of the fade out before a swap, and of the fade in after
    static constexpr int kLevelGrid = 256;  ///< samples between the points where a send takes its new level

    /// The first program, before any audio. False, with `error` set, when the plan names a slot it was given no
    /// instance for, or a buffer or pair it does not have.
    bool prepare(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
                 const Setup& setup, juce::String& error, Keep keep = {}, std::optional<std::vector<Route>> routes = std::nullopt);

    /// An instance new to the runner, readied for it before it is in any program: the playhead it reads, and that
    /// it plays in real time. Message thread; prepare() readies the first program's itself.
    void ready(juce::AudioProcessor* plugin);

    /// A new program, while audio runs, at the setup prepare() was given; the same checks. Every instance it names
    /// not in a program before must have been readied.
    bool swap(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots, int levels,
              juce::String& error, Keep keep = {}, std::optional<std::vector<Route>> routes = std::nullopt);

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
    /// prepared or swapped. Message thread: each entry is a value of its own, ordered against nothing, taken at
    /// the next grid point.
    void setLevel(int entry, float level) noexcept;

    /// The input row as the device hands it over: its channels referred to where they are, never copied; a null
    /// channel is silent.
    struct Inputs {
        const float* const* channels{nullptr};
        int count{0};
        const float* at(int channel, int start) const noexcept {
            return channel < count && channels[channel] != nullptr ? channels[channel] + start : nullptr;
        }
    };

    /// `frames` of the input row in, the decoders' and Main's out, played in pieces of at most `maxBlock`. Output
    /// channels nothing is routed to are silent.
    void process(const Inputs& in, juce::AudioBuffer<float>& output, int frames) noexcept;
    void process(const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& output, int frames) noexcept {
        process(Inputs{in.getArrayOfReadPointers(), in.getNumChannels()}, output, frames);
    }

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
        struct Level {
            std::atomic<float> target{1.0f};     ///< as last set
            mutable float from{1.0f}, to{1.0f};  ///< the ramp of this grid interval: the audio thread's
            mutable bool begun{false};           ///< a ramp has been taken; before it, the level is its target
        };
        std::unique_ptr<Level[]> levels;
        int levelCount{0};
        int longest{0};  ///< samples: the longest delay line
        std::vector<Route> routes;
        bool straight{false};  ///< given no routes: the first decoder straight out
        mutable std::vector<Line> routeLines;  // per route, one channel; Main's are the runner's
    };

    std::unique_ptr<Program> build(const live::Compiled& compiled, const std::vector<juce::AudioProcessor*>& slots,
                                   int levels, juce::String& error, Keep keep,
                                   std::optional<std::vector<Route>> routes);
    static std::vector<Route> mainOf(const std::vector<Route>& routes);
    /// Main into its lines, and out along the program's routes, at a gain ramping from `g0` to `g1`.
    void main(const Program& p, const Inputs& in, int inStart, juce::AudioBuffer<float>& output,
              int outStart, int frames, float g0, float g1) noexcept;
    void run(const Program& p, int slot, juce::AudioBuffer<float>& scratch) noexcept;
    /// A send's gain at `offset` samples into the current grid interval.
    static float gainAt(const Program::Level& l, int offset) noexcept {
        return l.from + (l.to - l.from) * static_cast<float>(offset) / static_cast<float>(kLevelGrid);
    }
    void piece(const Program& p, const Inputs& loop, int loopStart, juce::AudioBuffer<float>& output,
               int outStart, int frames, float fade0, float fade1) noexcept;

    Setup setup_;
    int field_{16};  // channels of the field at the plan's order
    Handoff<Program> handoff_;
    Program* latest_{nullptr};  // the program last prepared or swapped: the message thread's
    juce::MidiBuffer midi_;
    LivePlayhead playhead_;
    live::TransportProjector* clock_{nullptr};
    std::int64_t sample_{0};
    enum class Fade { None, Out, Hold, MainOut, In };
    Fade fade_{Fade::None};  // the audio thread's, with `faded_`: samples of the phase played
    int faded_{0};
    int hold_{0};  // samples of silence held between the fade out and the swap
    juce::AudioBuffer<float> mainLines_;  // Main's left and right, `maxMainDelay + maxBlock` samples
    int mainAt_{0};                       // where Main's next sample goes
    std::atomic<bool> mainMoves_{false};  // the program posted last plays Main otherwise than the one before
    bool mainFading_{false};              // the audio thread's: Main fades through this swap
};

}  // namespace bambi::livehost
