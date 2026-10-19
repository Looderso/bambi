// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>

namespace bambi::host {

/*  One bus of a block's buffer, read in place: the buffer's own channel pointers from where the bus starts.
 *  AudioProcessor::getBusBuffer copies those pointers into a new buffer, which allocates once a bus has 32
 *  channels; this allocates nothing, so it is how the audio thread reaches a bus. Valid for one block. */
struct BusChannels {
    float* const* data{nullptr};
    int count{0};    ///< channels, as many as both the layout and the buffer have
    int samples{0};  ///< the block's length

    const float* read(int channel) const noexcept { return data[channel]; }
    float* write(int channel) const noexcept { return data[channel]; }
    void clear(int channel) const noexcept { std::fill_n(data[channel], samples, 0.0f); }
    void clear() const noexcept {
        for (int c = 0; c < count; ++c) clear(c);
    }
    /// The largest magnitude on any channel.
    float peak() const noexcept {
        float m = 0.0f;
        for (int c = 0; c < count; ++c)
            for (int i = 0; i < samples; ++i) m = std::max(m, std::abs(data[c][i]));
        return m;
    }
};

/// Bus `bus` of the inputs, or of the outputs, in `buffer`. An absent bus has no channels. Audio thread.
inline BusChannels busChannels(const juce::AudioProcessor& p, juce::AudioBuffer<float>& buffer, bool isInput,
                               int bus) noexcept {
    if (bus < 0 || bus >= p.getBusCount(isInput)) return {};
    const int first = p.getChannelIndexInProcessBlockBuffer(isInput, bus, 0);
    const int count = std::clamp(buffer.getNumChannels() - first, 0, p.getChannelCountOfBus(isInput, bus));
    if (count == 0) return {nullptr, 0, buffer.getNumSamples()};
    return {buffer.getArrayOfWritePointers() + first, count, buffer.getNumSamples()};
}

}  // namespace bambi::host
