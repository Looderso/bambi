// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "bambi/live/transport.hpp"

/*  Live's timeline as the plugins read their host's: what bambi-live's TransportProjector said for the piece being
 *  played. The sample position is given as such, so a plugin never rounds it from seconds; the plugins' own rule for
 *  what is a locate stays theirs, and none is written here.
 *
 *  set() and getPosition() both run on the audio thread, the first before the plugins process, the second inside
 *  their processing, so the reading needs no guard.
 */
namespace bambi::livehost {

class LivePlayhead final : public juce::AudioPlayHead {
public:
    void set(const live::Playhead& reading, double rate) noexcept {
        reading_ = reading;
        rate_ = rate;
    }

    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(reading_.playing);
        info.setTimeInSamples(reading_.timeline);
        info.setTimeInSeconds(rate_ > 0.0 ? static_cast<double>(reading_.timeline) / rate_ : 0.0);
        info.setPpqPosition(reading_.ppq);
        info.setBpm(reading_.bpm);
        return info;
    }

private:
    live::Playhead reading_;
    double rate_{48000.0};
};

}  // namespace bambi::livehost
