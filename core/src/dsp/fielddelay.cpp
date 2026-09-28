// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/fielddelay.hpp"

#include <algorithm>
#include <cstring>

namespace bambi {

void FieldDelayLine::prepare(int channels, int capacityFrames) {
    channels_ = std::max(1, channels);
    capacity_ = std::max(1, capacityFrames);
    ring_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_),
                 0.0f);  // assign writes every page
    head_ = 0;
    written_ = 0;
}

void FieldDelayLine::write(const float* in, int frames) noexcept {
    const auto width = static_cast<std::size_t>(channels_);
    int done = 0;
    while (done < frames) {
        const int run = std::min(frames - done, capacity_ - head_);
        std::memcpy(ring_.data() + static_cast<std::size_t>(head_) * width, in + static_cast<std::size_t>(done) * width,
                    static_cast<std::size_t>(run) * width * sizeof(float));
        head_ = (head_ + run) % capacity_;
        done += run;
    }
    written_ += frames;
}

void FieldDelayLine::fetch(int back, int frames, float* out) const noexcept {
    const auto width = static_cast<std::size_t>(channels_);
    //  How many of the frames asked for fall before anything was written: those are silence.
    const std::int64_t missing = std::clamp<std::int64_t>(static_cast<std::int64_t>(back) - written_, 0, frames);
    const int silent = back > capacity_ ? frames : static_cast<int>(missing);
    if (silent > 0) std::memset(out, 0, static_cast<std::size_t>(silent) * width * sizeof(float));

    int done = silent;
    int at = ((head_ - back + done) % capacity_ + capacity_) % capacity_;
    while (done < frames) {
        const int run = std::min(frames - done, capacity_ - at);
        std::memcpy(out + static_cast<std::size_t>(done) * width, ring_.data() + static_cast<std::size_t>(at) * width,
                    static_cast<std::size_t>(run) * width * sizeof(float));
        at = (at + run) % capacity_;
        done += run;
    }
}

}  // namespace bambi
