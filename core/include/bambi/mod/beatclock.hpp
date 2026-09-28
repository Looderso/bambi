// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

/// Where the song is, in quarter notes, at a control step's sample. A step's position is solved from
/// an anchor -- a sample and the host's quarter note there -- plus its distance from it in whole
/// samples of the caller's own count, never from where the host began the block. The anchor moves
/// only on a new tempo or a drift past half a sample; a tempo of 0 holds the position, stopped.
namespace bambi {

class BeatClock {
public:
    /// Nothing known: the next reading is the anchor. A start, a locate, preparing to play.
    void forget() noexcept { known_ = false; }

    /// The host's reading at `sample`, the first sample of a block. Real-time safe.
    void observe(std::int64_t sample, double ppq, double bpm, double sampleRate) noexcept {
        if (known_ && ppq == lastPpq_ && bpm == lastBpm_) return;  // not moved: a stale sub-block, or stopped
        lastPpq_ = ppq;
        lastBpm_ = bpm;
        const double perSample = bpm / (60.0 * sampleRate);
        const bool sameTempo = std::abs(bpm - bpm_) <= 1e-9 * std::abs(bpm_);  // a tempo map's last bit is not a change
        if (known_ && sameTempo && std::abs(ppq - at(sample)) <= 0.5 * perSample) return;
        known_ = true;
        sample_ = sample;
        ppq_ = ppq;
        bpm_ = bpm;
        perSample_ = perSample;
    }

    /// Quarter notes at `sample`. Before any reading, 0.
    double ppqAt(std::int64_t sample) const noexcept { return known_ ? at(sample) : 0.0; }

private:
    double at(std::int64_t sample) const noexcept { return ppq_ + static_cast<double>(sample - sample_) * perSample_; }

    bool known_{false};
    std::int64_t sample_{0};
    double ppq_{0.0}, bpm_{120.0}, perSample_{0.0};
    double lastPpq_{0.0}, lastBpm_{0.0};
};

}  // namespace bambi
