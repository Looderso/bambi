// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "bambi/decode/allrad.hpp"
#include "bambi/link/latest.hpp"

/*  The loudspeaker output on the audio thread: the decoding matrix applied per frame, each output's
 *  distance gain folded into its row, then each output delayed by its distance to the farthest
 *  loudspeaker, in whole samples. A design arrives whole from a worker through a triple buffer; when
 *  one arrives the renderer keeps its own copy of the old one and fades over kSpeakerFadeSeconds: the
 *  old and the new design each feed their own delay lines, and the two delayed outputs are mixed with
 *  the ramp, so nothing steps and every output fades together. Both sets of lines are written all the
 *  time, so the old design's history is there when a new one comes; the ramp waits for the new
 *  design's longest delay, until its own lines hold its signal, and the next design is taken up only
 *  once the lines have settled the same way after a fade. Only the outputs a design feeds are written
 *  and read; a line a narrower design leaves is emptied once. Silence until the first design.
 */
namespace bambi {

inline constexpr double kSpeakerFadeSeconds = 0.01;

class SpeakerRenderer {
public:
    /// Allocates: room for maxOutputs rows of (maxOrder + 1)^2, and a delay line per output for maxDelaySeconds
    /// plus `latencyFrames`, which every output is held by on top of its own delay -- so the array lands where the
    /// convolved outputs land and one latency reported to the host is true for all. Never while publish() or
    /// process() may run.
    bool prepare(int maxOrder, int maxOutputs, double sampleRate, double maxDelaySeconds = 0.1, int latencyFrames = 0);

    /// Worker thread, one at a time: publishes a design. False, and nothing published, when it exceeds what prepare()
    /// made room for. Allocates nothing.
    bool publish(const SpeakerDesign& design) noexcept;

    /// Audio thread. `in`: frames x inChannels interleaved, AmbiX; `out`: frames x outChannels interleaved, output
    /// o on channel o; channels the design has no row for, and rows the output has no channel for, are silent.
    /// May not alias.
    void process(const float* in, int inChannels, float* out, int outChannels, int frames) noexcept;
    /// Empties the delay lines, keeps the design.
    void reset() noexcept;

    int fadeFrames() const { return fade_; }
    int maxOutputs() const { return maxOutputs_; }

private:
    /// A design as the audio thread plays it: rows with the gains in, one delay per output, in samples.
    struct Set {
        int outputs{0}, channels{0};
        /// maxChannels x maxOutputs: a channel's row over the outputs, so the per-frame multiply runs along
        /// contiguous outputs with no reduction. The stride is maxOutputs, whatever the design uses.
        std::vector<float> matrix;
        std::vector<int> delay;  ///< maxOutputs, in samples, the renderer's latency included
        int longestDelay{0};     ///< the largest of them, in samples
    };
    void mix(const Set& set, const float* x, int inChannels, float* y) const noexcept;

    int maxOutputs_{0}, maxChannels_{0}, ringLength_{0}, fade_{1}, maxDelay_{0}, latency_{0};
    double sampleRate_{48000.0};
    LatestValue<Set> sets_;
    const Set* live_{nullptr};
    Set leaving_;      ///< the renderer's copy of the set a new one replaces, for the fade
    int fadePos_{-1};  ///< frames into the ramp; negative while the new design's lines fill, and -1 when no fade runs
    bool fading_{false};
    int settle_{0};  ///< frames until the leaving lines hold the live design throughout, after a fade
    std::vector<float> ringLive_, ringLeave_;  ///< maxOutputs x ringLength each: an output before its delay
    int write_{0};
    std::vector<float> yLive_, yLeave_;  ///< maxOutputs each: one frame's outputs
};

/*  The loudspeaker path across its threads: request() from the message thread hands a layout, an
 *  order and the weighting to one worker, whose work() designs the decoder and publishes it; the
 *  audio thread calls process(). A layout the design refuses leaves the previous design playing and
 *  its reason in lastError(). The owner calls work() before the first block, so what plays, and what
 *  a bounce renders, is a function of the layout and never of the worker's timing.
 */
class SpeakerDecoder {
public:
    bool prepare(int maxOrder, int maxOutputs, double sampleRate, double maxDelaySeconds = 0.1, int latencyFrames = 0);

    /// Message thread, never the audio thread: it copies the layout.
    void request(const Layout& layout, int order, Weighting weighting, const Microphones& microphones = {});
    bool stale() const noexcept {
        return requested_.load(std::memory_order_acquire) != built_.load(std::memory_order_acquire);
    }
    /// Worker thread, one at a time: designs and publishes the newest request when there is one. True when a design
    /// was published; false when there was nothing new or the layout was refused (see lastError()). Allocates.
    bool work();
    /// The reason the last request was refused, empty when it was not. Read it when the worker is idle.
    std::string lastError() const;
    /// The design playing, as last published. Read it when the worker is idle.
    const SpeakerDesign& design() const { return design_; }

    void process(const float* in, int inChannels, float* out, int outChannels, int frames) noexcept {
        renderer_.process(in, inChannels, out, outChannels, frames);
    }
    void reset() noexcept { renderer_.reset(); }

private:
    SpeakerRenderer renderer_;
    SpeakerDesign design_;
    mutable std::mutex mutex_;  // the pending request and the error; never taken on the audio thread
    Layout layout_;
    int order_{1};
    Weighting weighting_{Weighting::Basic};
    Microphones microphones_;
    std::string error_;
    std::atomic<int> requested_{0}, built_{0};
};

}  // namespace bambi
