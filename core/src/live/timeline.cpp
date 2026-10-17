// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/timeline.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

std::string barsBeats(double ppq) {
    const double at = std::max(0.0, ppq);
    const auto bar = static_cast<long long>(at / kBeatsPerBar);
    const double inBar = at - static_cast<double>(bar) * kBeatsPerBar;
    const auto beat = static_cast<int>(inBar);
    const auto sixteenth = static_cast<int>((inBar - beat) * 4.0);
    return std::to_string(bar + 1) + "." + std::to_string(beat + 1) + "." + std::to_string(sixteenth + 1);
}

double songLength(const Arrangement& a, int least) {
    double end = 0.0;
    for (const TrackClips& t : a.tracks)
        for (const Clip& c : t.clips) end = std::max(end, c.end);
    for (const Locator& l : a.locators) end = std::max(end, l.time);
    end = std::max(end, a.loopStart + a.loopLength);
    const double bars = std::ceil(end / kBeatsPerBar);
    return std::max(bars, static_cast<double>(least)) * kBeatsPerBar;
}

double locateAt(double ppq, const Arrangement& a, double grid, double tolerance) {
    const Locator* near = nullptr;
    for (const Locator& l : a.locators)
        if (std::abs(l.time - ppq) <= tolerance && (near == nullptr || std::abs(l.time - ppq) < std::abs(near->time - ppq)))
            near = &l;
    if (near != nullptr) return near->time;
    return std::max(0.0, grid > 0.0 ? std::round(ppq / grid) * grid : ppq);
}

double heardAt(double ppq, double bpm, double rate, int samplesLate) {
    return rate > 0.0 ? ppq - static_cast<double>(samplesLate) * bpm / (60.0 * rate) : ppq;
}

}  // namespace bambi::live
