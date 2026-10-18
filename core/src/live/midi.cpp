// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/midi.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

bool MidiPending::add(const MidiEvent& e) noexcept {
    if (count_ == kCapacity) return false;
    //  kept in sample order: a later event, the usual case, goes on the end
    int at = count_;
    while (at > 0 && events_[static_cast<std::size_t>(at - 1)].sample > e.sample) {
        events_[static_cast<std::size_t>(at)] = events_[static_cast<std::size_t>(at - 1)];
        --at;
    }
    events_[static_cast<std::size_t>(at)] = e;
    ++count_;
    return true;
}

int MidiPending::take(std::int64_t end, MidiEvent* out, int room) noexcept {
    int n = 0;
    while (n < count_ && n < room && events_[static_cast<std::size_t>(n)].sample < end) {
        out[n] = events_[static_cast<std::size_t>(n)];
        ++n;
    }
    for (int i = n; i < count_; ++i) events_[static_cast<std::size_t>(i - n)] = events_[static_cast<std::size_t>(i)];
    count_ -= n;
    return n;
}

void MidiLead::add(double ms) noexcept {
    kept_[static_cast<std::size_t>(next_)] = ms;
    next_ = (next_ + 1) % kKept;
    count_ = std::min(count_ + 1, kKept);
}

double MidiLead::median() const noexcept {
    if (count_ == 0) return 0.0;
    std::array<double, kKept> v = kept_;
    std::sort(v.begin(), v.begin() + count_);
    return v[static_cast<std::size_t>(count_ / 2)];
}

double MidiLead::spread() const noexcept {
    if (count_ < 2) return 0.0;
    std::array<double, kKept> v = kept_;
    std::sort(v.begin(), v.begin() + count_);
    return (v[static_cast<std::size_t>(count_ * 3 / 4)] - v[static_cast<std::size_t>(count_ / 4)]) / 2.0;
}

void LeadMeter::note(std::int64_t at) noexcept {
    if (onset_ && at - *onset_ >= 0 && at - *onset_ <= window_) {  // the sound came first
        result_ = *onset_ - at;
        onset_.reset();
        return;
    }
    armed_ = at;
}

void LeadMeter::listen(const float* left, const float* right, int frames, std::int64_t start) noexcept {
    for (int i = 0; i < frames; ++i) {
        const float v =
            std::max(left != nullptr ? std::abs(left[i]) : 0.0f, right != nullptr ? std::abs(right[i]) : 0.0f);
        const std::int64_t at = start + i;
        if (armed_ && at - *armed_ > window_) armed_.reset();
        if (v <= threshold_) {
            ++silent_;
            continue;
        }
        if (silent_ >= quiet_) {
            if (armed_ && at >= *armed_) {
                result_ = at - *armed_;
                armed_.reset();
            } else {
                onset_ = at;
            }
        }
        silent_ = 0;
    }
}

std::optional<std::int64_t> LeadMeter::took() noexcept {
    const auto r = result_;
    result_.reset();
    return r;
}

}  // namespace bambi::live
