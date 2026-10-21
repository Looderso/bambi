// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <vector>

namespace bambi::ui {

/// What one channel shows: what plays there, in its owner's ink, or nothing.
struct ChannelCell {
    juce::String label;                  ///< its number when empty; what plays there otherwise (L, R, 1, ...)
    std::optional<juce::Colour> fill;    ///< what plays there, in its owner's ink; none: empty
    std::optional<juce::Colour> dashed;  ///< outlined, dashed: the host's own copy, an output not playing
    int clash{0};                        ///< two or more things on it: how many
};

/*  A run of channels, however many, every cell the same normal size. In one row, in groups with a gap between, as
 *  the Decoder's map lays them out: a run wider than the space is a window onto it, moved sideways with the wheel, a
 *  thin bar under it saying where the window is. Or wrapped into lines, as the live host's device cards lay them
 *  out where the page has the room to show every channel. Every cell is painted by `paintCell`, from what the caller
 *  says plays there; where to press stays the caller's, inside `viewport`. */
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

    /// One cell, as every run of channels in the suite paints it: the panel's ground, or its owner's ink; a clash
    /// framed strong with its count; the label centred.
    static void paintCell(juce::Graphics& g, juce::Rectangle<float> box, const ChannelCell& cell);

    /// The bar under the row, when the run is wider than the viewport.
    void paintBar(juce::Graphics& g, juce::Colour ink, float scroll) const;

private:
    std::vector<std::size_t> firstOfGroup_;
};

}  // namespace bambi::ui
