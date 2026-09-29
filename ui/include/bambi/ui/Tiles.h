// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "bambi/patch/parameters.hpp"
#include "bambi/ui/Theme.h"

/// The tile, and the grid it sits in. Every parameter page in the suite is a grid of these, two to
/// a row: a name, a number, and one bar carrying what is set, what modulation reaches, and what the
/// engine is applying. A tile knows nothing about parameters -- what to show is decided by the
/// caller and handed over as a TileContent, which is what lets it carry a host parameter, a piece
/// of state, or a pair of values shown as one range.
namespace bambi::ui {

/// A value: name, number, and a bar -- with the modulation range in blue.
struct TileContent {
    juce::String name, value;
    float valueSize{theme::type::value};
    double fillFrom{0.0}, fillTo{0.0};  ///< normalised; a range fills between two values
    double modFrom{0.0}, modTo{0.0};    ///< normalised modulation reach; equal means none
    bool nameActive{false};             ///< committed or provisional: the name in ink
    bool committed{false};              ///< underlined in the modulation colour
    bool highlighted{false};            ///< the provisional parameter
    bool enabled{true};
    bool modulationInk{false};  ///< a source's amount: number and bar in the modulation colour
    bool tick{false};           ///< the value is signed: it fills from its zero
    double live{0.0};           ///< normalised; where the engine has it now
    bool hasLive{false};        ///< it differs from the set value: mark it
};

float tileHeight(float valueSize);

/// The box a value lives in: drag anywhere in it to set the value, click it to give it a matrix row.
/// One target, so the name and the bar read as a label and a reading, not two separate controls.
juce::Rectangle<float> tileBox(juce::Rectangle<float> tile);
/// Where a tile's bar is drawn: along its bottom edge, the tile's width.
juce::Rectangle<float> tileBar(juce::Rectangle<float> tile);
/// Draws the tile and returns the region that moves on its own: the live mark's extent, past the tile's bottom edge.
juce::Rectangle<float> drawTile(juce::Graphics& g, juce::Rectangle<float> tile, const TileContent& t);

/// The `index`th tile of a two-column grid starting at `y`, and where a grid of `count` tiles ends.
juce::Rectangle<float> tileCell(juce::Rectangle<float> content, float y, int index,
                                float valueSize = theme::type::value);
float tilesBottom(float y, int count, float valueSize = theme::type::value);

/// A parameter's value across its range, 0 to 1. `m` is the plugin's own manifest.
double normalised(const ParamManifest& m, ParamId id, double value);

}  // namespace bambi::ui
