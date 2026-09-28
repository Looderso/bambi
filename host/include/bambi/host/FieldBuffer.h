// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

#include "bambi/math/sh.hpp"

/*  Between a host's planar channels and an engine's interleaved frames.
 *
 *  Both field engines take interleaved frames, because the kernels underneath them work a frame at a
 *  time across channels (`dsp/field.hpp`) -- a planar entry point would gather inside the engine
 *  instead of outside it and save nothing. A host hands over planar channels. So every field effect
 *  copies, at every segment, and the copy is written once here rather than once per plugin.
 */
namespace bambi::host {

class FieldBuffer {
public:
    /// Allocates for the longest segment at this order. Nothing after this allocates.
    void prepare(int maxFrames, int order) {
        channels_ = bambi::numChannels(std::max(0, order));
        frames_ = std::max(0, maxFrames);
        storage_.assign(static_cast<std::size_t>(frames_) * static_cast<std::size_t>(channels_), 0.0f);
    }

    int channels() const noexcept { return channels_; }
    int capacity() const noexcept { return frames_; }

    float* data() noexcept { return storage_.data(); }
    const float* data() const noexcept { return storage_.data(); }

    /// `frames` frames from `at` of a planar bus, interleaved. Real-time safe. Reading more frames than were prepared for, or a bus narrower than the order, is refused by taking the smaller.
    void read(const juce::AudioBuffer<float>& from, int at, int frames) noexcept {
        const int n = std::min(frames, frames_);
        const int c = std::min(channels_, from.getNumChannels());
        for (int ch = 0; ch < c; ++ch) {
            const float* x = from.getReadPointer(ch) + at;
            for (int i = 0; i < n; ++i)
                storage_[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels_) +
                         static_cast<std::size_t>(ch)] = x[i];
        }
    }

    /// The same frames back out to a planar bus at `at`. Real-time safe.
    void write(juce::AudioBuffer<float>& to, int at, int frames) const noexcept {
        const int n = std::min(frames, frames_);
        const int c = std::min(channels_, to.getNumChannels());
        for (int ch = 0; ch < c; ++ch) {
            float* y = to.getWritePointer(ch) + at;
            for (int i = 0; i < n; ++i)
                y[i] = storage_[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels_) +
                                static_cast<std::size_t>(ch)];
        }
    }

private:
    std::vector<float> storage_;
    int channels_{0}, frames_{0};
};

}  // namespace bambi::host
