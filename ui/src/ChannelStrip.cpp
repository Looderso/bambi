// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/ChannelStrip.h"

#include <algorithm>
#include <numeric>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

namespace cs = theme::channelStrip;

ChannelStrip ChannelStrip::layout(float x, float top, float width, const std::vector<int>& groups) {
    ChannelStrip s;
    const int n = std::accumulate(groups.begin(), groups.end(), 0);
    const float gaps = groups.empty() ? 0.0f : static_cast<float>(groups.size() - 1) * cs::groupGap;
    const float cell = std::clamp((width - gaps) / static_cast<float>(std::max(n, 1)) - cs::cellGap, 1.0f, cs::maxCell);
    const float digits = textWidth(smallFont(), juce::String::repeatedString("0", cs::widest - 1));
    s.numbered = cell >= digits + 2.0f * cs::numberPad;
    s.cells.reserve(static_cast<std::size_t>(n));
    float at = x;
    for (int count : groups) {
        s.firstOfGroup_.push_back(s.cells.size());
        for (int i = 0; i < count; ++i, at += cell + cs::cellGap) s.cells.push_back({at, top, cell, cs::height});
        at += cs::groupGap - cs::cellGap;
    }
    s.bottom = top + cs::height;
    const float pitch = cell + cs::cellGap;
    const float room = textWidth(smallFont(), juce::String::repeatedString("0", cs::widest)) + cs::tickPad;
    //  a step the numbers read in: 1, 2, 4, 8 ... until two numbers clear each other
    while (s.step * pitch < room && s.step < static_cast<float>(std::max(n, 1))) s.step *= 2.0f;
    return s;
}

float ChannelStrip::groupLeft(std::size_t group) const {
    return group < firstOfGroup_.size() && firstOfGroup_[group] < cells.size() ? cells[firstOfGroup_[group]].getX()
                                                                               : 0.0f;
}

float ChannelStrip::paintRuler(juce::Graphics& g, juce::Colour ink) const {
    if (numbered || cells.empty()) return bottom;
    const float width = textWidth(smallFont(), juce::String::repeatedString("0", cs::widest));
    const int every = static_cast<int>(step);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const int ch = static_cast<int>(i) + 1;
        if (ch != 1 && ch % every != 0) continue;
        const auto c = cells[i].getCentreX();
        text(g, juce::String(ch), smallFont(), ink, {c - width, bottom, 2.0f * width, cs::rulerLine},
             juce::Justification::centredTop);
    }
    return bottom + cs::rulerLine;
}

int ChannelStrip::at(juce::Point<float> p) const {
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (cells[i].expanded(cs::cellGap / 2.0f).contains(p)) return static_cast<int>(i);
    return -1;
}

}  // namespace bambi::ui
