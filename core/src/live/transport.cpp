// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/transport.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

double TransportProjector::ppqAt(std::int64_t sample) const noexcept {
    return playing_ ? ppq_ + static_cast<double>(sample - sample_) * perSample_ : ppq_;
}

std::int64_t TransportProjector::timelineAt(std::int64_t sample) const noexcept {
    return playing_ ? timeline_ + (sample - sample_) : timeline_;
}

std::int64_t TransportProjector::samplesOf(double ppq, double bpm) const noexcept {
    return static_cast<std::int64_t>(std::llround(ppq / nominal(bpm)));
}

void TransportProjector::anchor(std::int64_t at, double ppq, std::int64_t timeline, double perSample) noexcept {
    sample_ = at;
    ppq_ = ppq;
    timeline_ = timeline;
    perSample_ = perSample;
}

void TransportProjector::receive(const TransportMessage& m, std::int64_t at) noexcept {
    const double bpm = m.bpm > 0.0 ? m.bpm : bpm_;
    const double rate = nominal(bpm);
    //  what was already read stays read: the message takes effect from the later of its sample and the last read
    const std::int64_t here = std::max(at, read_);
    const double live = m.ppq + (m.playing ? static_cast<double>(here - at) * rate : 0.0);  // where Live is there

    if (!m.playing) {  // stopped: held where Live says, the timeline where it got to
        anchor(here, m.ppq, known_ ? timelineAt(here) : samplesOf(m.ppq, bpm), rate);
        playing_ = false;
    } else if (!(known_ && playing_)) {  // a start: the timeline from the song position
        anchor(here, live, samplesOf(live, bpm), rate);
        playing_ = true;
    } else {
        const double error = live - ppqAt(here);  // quarter notes Live is ahead of the projection
        if (std::abs(error) > static_cast<double>(s_.slackSamples) * rate) {  // a locate
            anchor(here, live, samplesOf(live, bpm), rate);
        } else {  // closed gradually from where the projection is, so nothing jumps; a new tempo applies from here
            const double skew =
                std::clamp(error / (static_cast<double>(s_.catchUpSamples) * rate), -s_.maxSkew, s_.maxSkew);
            anchor(here, ppqAt(here), timelineAt(here), rate * (1.0 + skew));
        }
    }
    bpm_ = bpm;
    known_ = true;
}

Playhead TransportProjector::reading(std::int64_t sample) noexcept {
    read_ = std::max(read_, sample);
    return {playing_, timelineAt(sample), ppqAt(sample), bpm_};
}

}  // namespace bambi::live
