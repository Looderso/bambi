// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/timeline.hpp"

#include <algorithm>
#include <cmath>

namespace bambi::live {

TimeView fitted(TimeView view, double length) {
    const double longest = std::max(kNarrowestView, length);
    view.span = std::clamp(view.span, std::min(kNarrowestView, longest), longest);
    view.start = std::clamp(view.start, 0.0, std::max(0.0, length - view.span));
    return view;
}

TimeView zoomed(const TimeView& view, double factor, double anchor, double length) {
    const double span = view.span * factor;
    const double at = view.span > 0.0 ? (anchor - view.start) / view.span : 0.0;  // where the anchor is drawn, 0..1
    const TimeView sized = fitted({view.start, span}, length);
    return fitted({anchor - at * sized.span, sized.span}, length);
}

TimeView scrolled(const TimeView& view, double beats, double length) {
    return fitted({view.start + beats, view.span}, length);
}

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

double snapAt(double ppq, const std::vector<double>& edges, double grid, double tolerance) {
    const double* near = nullptr;
    for (const double& e : edges)
        if (std::abs(e - ppq) <= tolerance && (near == nullptr || std::abs(e - ppq) < std::abs(*near - ppq))) near = &e;
    if (near != nullptr) return std::max(0.0, *near);
    return std::max(0.0, grid > 0.0 ? std::round(ppq / grid) * grid : ppq);
}

std::vector<double> edgesOf(const Arrangement& a, TrackId track) {
    std::vector<double> edges;
    for (const Locator& l : a.locators) edges.push_back(l.time);
    for (const TrackClips& t : a.tracks)
        if (track != 0 && t.id == track)
            for (const Clip& c : t.clips) {
                edges.push_back(c.start);
                edges.push_back(c.end);
            }
    return edges;
}

double locateAt(double ppq, const Arrangement& a, double grid, double tolerance) {
    return snapAt(ppq, edgesOf(a, 0), grid, tolerance);
}

double gridFor(double pixelsPerBeat) {
    return pixelsPerBeat >= 48.0 ? 0.25 : pixelsPerBeat >= 12.0 ? 1.0 : static_cast<double>(kBeatsPerBar);
}

double heardAt(double ppq, double bpm, double rate, int samplesLate) {
    return rate > 0.0 ? ppq - static_cast<double>(samplesLate) * bpm / (60.0 * rate) : ppq;
}

}  // namespace bambi::live
