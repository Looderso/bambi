// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/ChannelStrip.h"

#include <algorithm>
#include <numeric>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

namespace cs = theme::channelStrip;

ChannelStrip ChannelStrip::layout(float x, float top, float width, const std::vector<int>& groups, float& scroll,
                                  int reveal) {
    ChannelStrip s;
    const int n = std::accumulate(groups.begin(), groups.end(), 0);
    const float pitch = cs::cell + cs::cellGap;
    s.contentWidth =
        static_cast<float>(n) * pitch + static_cast<float>(std::max<std::size_t>(groups.size(), 1) - 1) * cs::groupGap;
    s.viewport = {x, top, width, cs::cell};
    const float limit = std::max(0.0f, s.contentWidth - width);
    float at = 0.0f;
    std::vector<float> lefts;
    for (int count : groups) {
        s.firstOfGroup_.push_back(lefts.size());
        for (int i = 0; i < count; ++i, at += pitch) lefts.push_back(at);
        at += cs::groupGap;
    }
    if (reveal >= 1 && reveal <= n) {
        const float left = lefts[static_cast<std::size_t>(reveal - 1)];
        if (left < scroll) scroll = left;
        if (left + cs::cell > scroll + width) scroll = left + cs::cell - width;
    }
    scroll = std::clamp(scroll, 0.0f, limit);
    s.cells.reserve(lefts.size());
    for (float left : lefts) s.cells.push_back({x + left - scroll, top, cs::cell, cs::cell});
    s.bottom = top + cs::cell + (limit > 0.0f ? cs::barGap + cs::bar : 0.0f);
    return s;
}

ChannelStrip ChannelStrip::wrapped(float x, float top, float width, int count, float linePitch) {
    ChannelStrip s;
    const float pitch = cs::cell + cs::cellGap;
    const int perLine = std::max(1, static_cast<int>((width + cs::cellGap) / pitch));
    const int lines = std::max(1, (count + perLine - 1) / perLine);
    s.contentWidth = width;
    s.viewport = {x, top, width, static_cast<float>(lines - 1) * linePitch + cs::cell};
    s.firstOfGroup_.push_back(0);
    s.cells.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i)
        s.cells.push_back({x + static_cast<float>(i % perLine) * pitch,
                           top + static_cast<float>(i / perLine) * linePitch, cs::cell, cs::cell});
    s.bottom = s.viewport.getBottom();
    return s;
}

float ChannelStrip::groupLeft(std::size_t group) const {
    return group < firstOfGroup_.size() && firstOfGroup_[group] < cells.size() ? cells[firstOfGroup_[group]].getX()
                                                                               : viewport.getX();
}

void ChannelStrip::paintCell(juce::Graphics& g, juce::Rectangle<float> box, const ChannelCell& cell) {
    namespace colour = theme::colour;
    const bool clash = cell.clash > 1;
    g.setColour(!clash && cell.fill ? *cell.fill : colour::panel);
    g.fillRect(box);
    if (clash) {
        g.setColour(colour::ruleStrong);
        g.drawRect(box, theme::stroke::rule);
    } else if (cell.dashed) {
        juce::Path edge;
        edge.addRectangle(box.reduced(theme::stroke::toggle / 2.0f));
        g.setColour(*cell.dashed);
        strokeDashed(g, edge, theme::stroke::unplacedDash, juce::PathStrokeType(theme::stroke::toggle));
    }
    const juce::String label = clash ? juce::String::fromUTF8("\xc3\x97") + juce::String(cell.clash) : cell.label;
    text(g, label, smallFont(),
         !clash && cell.fill    ? colour::textInverse
         : clash || cell.dashed ? colour::text
                                : colour::label,
         box, juce::Justification::centred);
}

void ChannelStrip::paintBar(juce::Graphics& g, juce::Colour ink, float scroll) const {
    if (contentWidth <= viewport.getWidth()) return;
    const float y = viewport.getBottom() + cs::barGap;
    const float thumb = viewport.getWidth() * viewport.getWidth() / contentWidth;
    const float travel = viewport.getWidth() - thumb;
    const float at = travel * scroll / (contentWidth - viewport.getWidth());
    g.setColour(ink);
    g.fillRect(juce::Rectangle<float>{viewport.getX() + at, y, thumb, cs::bar});
}

}  // namespace bambi::ui
