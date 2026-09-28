// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace bambi::host {

/// A transport for checks, benches and tools: playing (or stopped) at `bpm`, at sample `time`.
struct TestPlayHead final : juce::AudioPlayHead {
    double rate{48000.0};
    double bpm{120.0};
    juce::int64 time{0};
    bool playing{true};

    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setIsPlaying(playing);
        p.setTimeInSamples(time);
        p.setBpm(bpm);
        p.setPpqPosition(static_cast<double>(time) / rate * (bpm / 60.0));
        return p;
    }
};

}  // namespace bambi::host
