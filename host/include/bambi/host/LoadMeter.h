// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>

/*  A plugin's own DSP load, measured where it runs, as a share of each block's real-time budget. A
 *  host's CPU meter cannot tell this apart from everything else the host is doing, so every
 *  processor measures it itself, the same way.
 *
 *  `begin` and `end` are called on the audio thread, around the block. They read a clock and store
 *  two atomics: no allocation, no lock. `load` and `peak` are read from anywhere.
 */
namespace bambi::host {

class LoadMeter {
public:
    void begin() noexcept { started_ = juce::Time::getHighResolutionTicks(); }

    void end(int numSamples, double sampleRate) noexcept {
        const auto finished = juce::Time::getHighResolutionTicks();
        const double budget = sampleRate > 0.0 ? static_cast<double>(numSamples) / sampleRate : 0.0;
        if (budget > 0.0) {
            const double now = juce::Time::highResolutionTicksToSeconds(finished - started_) / budget * 100.0;
            smoothed_ += (now - smoothed_) * kSmoothing;
            worst_ = std::max(worst_, now);
        }
        load_.store(static_cast<float>(smoothed_), std::memory_order_relaxed);

        //  the peak is of the last second, so a spike is seen and then let go
        sincePeak_ += numSamples;
        if (sincePeak_ >= static_cast<int>(sampleRate)) {
            peak_.store(static_cast<float>(worst_), std::memory_order_relaxed);
            worst_ = 0.0;
            sincePeak_ = 0;
        }
    }

    /// Percent of a block's budget, smoothed; and the worst single block of the last second.
    float load() const noexcept { return load_.load(std::memory_order_relaxed); }
    float peak() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    static constexpr double kSmoothing = 0.05;

    std::int64_t started_{0};
    double smoothed_{0.0}, worst_{0.0};
    int sincePeak_{0};
    std::atomic<float> load_{0.0f}, peak_{0.0f};
};

}  // namespace bambi::host
