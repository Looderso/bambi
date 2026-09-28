// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

#include "bambi/host/TestPlayHead.h"

/*  What a whole `processBlock` costs, for a tool to print.
 *
 *  `bambi-bench` times the engines in `core/`; this times a plugin as a host drives it -- the
 *  parameter intake, the detectors, the control grid, the copy in and out -- against a playing
 *  transport, in the build the tool was compiled in. A Debug build's numbers mean nothing, and the
 *  caller says so. Not for the audio thread: it allocates.
 */
namespace bambi::host {

struct BlockStats {
    double medianUs{0.0}, p99Us{0.0}, maxUs{0.0}, coreShare{0.0};
};

/// `seconds` of noise on every input channel through a processor whose layout is already set. Prepares and releases it.
inline BlockStats benchBlocks(juce::AudioProcessor& proc, double rate, int block, double seconds = 10.0) {
    using Clock = std::chrono::steady_clock;
    TestPlayHead head;
    head.rate = rate;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(rate, block);
    proc.prepareToPlay(rate, block);

    const int channels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    const int inputs = proc.getTotalNumInputChannels();
    juce::AudioBuffer<float> buffer(channels, block);
    juce::MidiBuffer midi;
    //  an LCG and not <random>: a distribution's output is the library's, and differs between them
    std::uint32_t seed = 7;
    const auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
    };

    const int blocks = static_cast<int>(seconds * rate / block);
    constexpr int warmup = 50;
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(blocks));
    double total = 0.0;
    for (int b = 0; b < blocks + warmup; ++b) {
        buffer.clear();
        for (int c = 0; c < inputs; ++c)
            for (int i = 0; i < block; ++i) buffer.setSample(c, i, noise());
        const auto t0 = Clock::now();
        proc.processBlock(buffer, midi);
        const double t = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        head.time += block;
        if (b >= warmup) {
            us.push_back(t);
            total += t;
        }
    }
    proc.releaseResources();
    proc.setPlayHead(nullptr);

    std::sort(us.begin(), us.end());
    BlockStats out;
    out.medianUs = us[us.size() / 2];
    out.p99Us = us[us.size() * 99 / 100];
    out.maxUs = us.back();
    out.coreShare = total / (static_cast<double>(blocks) * block / rate * 1e6) * 100.0;
    return out;
}

inline void printBlockStats(int order, int block, const BlockStats& s) {
    std::printf("  %5d  %4d  %5d  %8.3f us  %8.3f us  %8.3f us  %8.3f%%\n", order, (order + 1) * (order + 1), block,
                s.medianUs, s.p99Us, s.maxUs, s.coreShare);
}

}  // namespace bambi::host
