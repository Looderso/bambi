// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

#include "PluginProcessor.h"
#include "bambi/link/link.hpp"

namespace bambi::ui {

/*  A timing trace of the scene, for finding out why it stutters. Diagnostics only.

    A source's position passes through three clocks before it is drawn: its audio thread computes it once per
    control step, whenever the host delivers a block; its instance's link timer publishes the newest; this
    editor's timer reads the bus and the globe paints. A frame that stands still stood still in one of those.
    For twenty seconds this records every link of that chain it can see -- each audio block's arrival and this
    instance's link ticks, each editor frame and globe paint, and for every instance in every frame the
    position shown with when it was computed and when published -- and writes it to /tmp as CSV. Every time
    is linkNowMicros, the one clock all instances share, relative to the start of the trace. */
class FrameTrace {
public:
    explicit FrameTrace(BambiEncoderProcessor& p) : processor_(p) {}
    ~FrameTrace();

    /// Record for `seconds` (a shorter run is for tests).
    void start(double seconds = 20.0);
    bool recording() const { return recording_; }

    /// Once per editor frame, right after the scene was read from the bus. Nothing while not recording.
    void frame(const bambi::LinkScene& scene);
    /// As the globe starts painting. Nothing while not recording.
    void painted();

    /// What to show: how to start, that it is recording, or where the file went.
    const juce::String& status() const { return status_; }

private:
    void finish();
    std::int64_t since(std::uint64_t us) const;

    BambiEncoderProcessor& processor_;
    bool recording_{false};
    std::uint64_t startUs_{0}, endUs_{0};
    std::int64_t frames_{0};
    std::string rows_;
    std::vector<BambiEncoderProcessor::TraceEvent> events_;
    juce::String status_;
};

}  // namespace bambi::ui
