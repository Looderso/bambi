// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace bambi::ui {

/*  A run of channels in one row, however many: the live host's channel map and the decoder's bus both lay their
 *  cells out with it. Never wrapped, never scrolled: the cells narrow to fit the width, in groups with a gap
 *  between. A cell wide enough for its number carries it; narrower, the numbers move to a ruler under the row,
 *  thinned to as many as fit. What a cell shows -- who plays there, a clash -- stays the caller's to draw. */
struct ChannelStrip {
    std::vector<juce::Rectangle<float>> cells;  ///< one per channel, in order across the groups
    bool numbered{true};                        ///< the cells are wide enough to carry their own number
    float bottom{0.0f};                         ///< under the cells
    float step{1};                              ///< when not `numbered`, every `step`th channel gets a number

    /// `groups` is the channel count of each group, left to right; `top` is the cells' top.
    static ChannelStrip layout(float x, float top, float width, const std::vector<int>& groups);

    /// The x of group `g`'s first cell, for a name over the group.
    float groupLeft(std::size_t group) const;

    /// The ruler under the row when the cells are not `numbered`: channel 1, then each `step`th. Returns the y under it.
    float paintRuler(juce::Graphics& g, juce::Colour ink) const;

    /// The 0-based channel whose cell holds `p`, or -1.
    int at(juce::Point<float> p) const;

private:
    std::vector<std::size_t> firstOfGroup_;
};

}  // namespace bambi::ui
