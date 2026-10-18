// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/clips.hpp"

#include "bambi/live/lanes.hpp"

namespace bambi::live {

const ClipClock::Entry* ClipClock::find(TrackId track) const noexcept {
    for (std::size_t i = 0; i < used_; ++i)
        if (entries_[i].track == track) return &entries_[i];
    return nullptr;
}

void ClipClock::receive(const SlotPlay& report, std::int64_t at) noexcept {
    for (std::size_t i = 0; i < used_; ++i)
        if (entries_[i].track == report.id) {
            entries_[i] = {report.id, report, at};
            return;
        }
    if (used_ < kTracks) entries_[used_++] = {report.id, report, at};
}

int ClipClock::playing(TrackId track) const noexcept {
    const Entry* e = find(track);
    return e != nullptr ? e->report.slot : -1;
}

std::optional<double> ClipClock::position(TrackId track, int slot, std::int64_t sample, double bpm,
                                          double sampleRate) const noexcept {
    const Entry* e = find(track);
    if (e == nullptr || e->report.slot != slot || slot < 0 || sampleRate <= 0.0) return std::nullopt;
    const double elapsed = static_cast<double>(sample - e->at) * bpm / (60.0 * sampleRate);
    return clipPosition(e->report, elapsed);
}

}  // namespace bambi::live
