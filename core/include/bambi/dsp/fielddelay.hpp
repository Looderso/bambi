// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

/*  A delay line over a whole field: interleaved frames in a ring, allocated once for the longest
 *  delay. A delay is a distance back from the write head. restart() forgets what was written in
 *  O(1) instead of clearing up to tens of megabytes.
 */
namespace bambi {

class FieldDelayLine {
public:
    /// Allocates and touches every page, so the audio thread never takes a first-touch fault.
    void prepare(int channels, int capacityFrames);

    /// Everything written so far is gone. O(1); real-time safe.
    void restart() noexcept { written_ = 0; }

    /// Append `frames` interleaved frames at the head.
    void write(const float* in, int frames) noexcept;

    /// out[k] is the frame at head - back + k, for `frames` frames; frames <= back <= capacity().
    /// Anything from before the last restart reads as silence.
    void fetch(int back, int frames, float* out) const noexcept;

    int channels() const { return channels_; }
    int capacity() const { return capacity_; }
    std::int64_t written() const { return written_; }  ///< frames since the last restart

private:
    std::vector<float> ring_;
    int channels_{0}, capacity_{0};
    int head_{0};  ///< where the next frame goes
    std::int64_t written_{0};
};

}  // namespace bambi
