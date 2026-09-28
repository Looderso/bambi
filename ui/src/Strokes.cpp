// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Strokes.h"

#include <algorithm>

#include "bambi/ui/Theme.h"

namespace bambi::ui {

namespace {
int issued = 0;  // the message thread's alone
}  // namespace

int strokesIssued() { return issued; }

void strokeSegments(juce::Graphics& g, const std::vector<ScreenSegment>& segments,
                    std::vector<juce::Point<double>>& skipped, juce::Colour colour, float width, bool dashed) {
    /*  A point is drawn only where leaving it out would move the line by more than the tolerance: a straight
        stretch becomes one element, a tight curve keeps what it needs, keeping the element count the platform
        strokes low. The last point of every run is kept, so ends, seams and the far hemisphere's edge stay put. */
    const auto joins = [](const ScreenSegment& a, const ScreenSegment& b) {
        return a.front == b.front && juce::exactlyEqual(a.x1, b.x0) && juce::exactlyEqual(a.y1, b.y0);
    };
    const auto offLine = [](juce::Point<double> p, juce::Point<double> a, juce::Point<double> b) {
        const auto ab = b - a;
        const double len2 = ab.x * ab.x + ab.y * ab.y;
        const double t = len2 > 0.0 ? std::clamp(((p - a).x * ab.x + (p - a).y * ab.y) / len2, 0.0, 1.0) : 0.0;
        return p.getDistanceFrom(a + ab * t);
    };
    const auto tolerance = static_cast<double>(theme::scene::strokeTolerance);
    const auto maxChord = static_cast<double>(theme::scene::strokeMaxChord);
    juce::Path front, back;
    juce::Point<double> last;
    skipped.clear();
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& s = segments[i];
        auto& path = s.front ? front : back;
        if (i == 0 || !joins(segments[i - 1], s)) {
            path.startNewSubPath(static_cast<float>(s.x0), static_cast<float>(s.y0));
            last = {s.x0, s.y0};
            skipped.clear();
        }
        const juce::Point<double> next{s.x1, s.y1};
        if (i + 1 < segments.size() && joins(s, segments[i + 1])) {
            //  Can one straight line from the last point drawn to this one stand for every point passed over?
            bool fits = next.getDistanceFrom(last) <= maxChord && skipped.size() < theme::scene::strokeRunCap;
            for (std::size_t k = 0; fits && k < skipped.size(); ++k)
                fits = offLine(skipped[k], last, next) <= tolerance;
            if (!fits && !skipped.empty()) {
                const auto corner = skipped.back();  // no: draw up to the point before, and start again from it
                path.lineTo(static_cast<float>(corner.x), static_cast<float>(corner.y));
                last = corner;
                skipped.clear();
            }
            skipped.push_back(next);
            continue;
        }
        path.lineTo(static_cast<float>(next.x), static_cast<float>(next.y));  // the run's end is always drawn
        last = next;
        skipped.clear();
    }

    const juce::PathStrokeType stroke(width);
    const auto draw = [&](const juce::Path& path, juce::Colour c) {
        if (path.isEmpty()) return;
        ++issued;
        g.setColour(c);
        if (dashed) {
            juce::Path dashes;
            stroke.createDashedStroke(dashes, path, theme::stroke::otherPathDash, 2);
            g.fillPath(dashes);
        } else {
            g.strokePath(path, stroke);
        }
    };
    draw(back, colour.withMultipliedAlpha(theme::scene::backAlpha));
    draw(front, colour);
}

}  // namespace bambi::ui
