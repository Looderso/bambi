// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace bambi::ui {

/*  A run of channels, however many, every cell the same normal size. In one row, in groups with a gap between, as
 *  the Decoder's map lays them out: a run wider than the space is a window onto it, moved sideways with the wheel, a
 *  thin bar under it saying where the window is. Or wrapped into lines, as the live host's device cards lay them
 *  out where the page has the room to show every channel. What a cell shows -- who plays there, a clash -- stays the
 *  caller's to draw, inside `viewport`. */
struct ChannelStrip {
    std::vector<juce::Rectangle<float>> cells;  ///< one per channel in order, scrolled: some lie outside `viewport`
    juce::Rectangle<float> viewport;            ///< where the row shows
    float contentWidth{0.0f};                   ///< the whole run
    float bottom{0.0f};                         ///< under the cells and the bar

    /// `groups` is the channel count of each group, left to right. `scroll` is how far the run is moved left, kept
    /// within the run; `reveal`, a channel from 1 (0 for none), is moved into view when it is not.
    static ChannelStrip layout(float x, float top, float width, const std::vector<int>& groups, float& scroll,
                               int reveal = 0);

    /// `count` channels in lines that wrap at `width`, each line `linePitch` below the last: every cell in view.
    static ChannelStrip wrapped(float x, float top, float width, int count, float linePitch);

    /// The x of group `group`'s first cell, for a name over the group.
    float groupLeft(std::size_t group) const;

    /// A cell's part that shows; empty for one scrolled out.
    juce::Rectangle<float> shown(std::size_t cell) const { return cells[cell].getIntersection(viewport); }

    /// The bar under the row, when the run is wider than the viewport.
    void paintBar(juce::Graphics& g, juce::Colour ink, float scroll) const;

private:
    std::vector<std::size_t> firstOfGroup_;
};

}  // namespace bambi::ui
