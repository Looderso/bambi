// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <string_view>
#include <vector>

#include "bambi/mod/manifest.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

/// The drawing every plugin's window does the same way. Nothing here knows about a patch, a parameter or a processor.

/// The one bar every value in this window is drawn with: a track, a fill, and the range modulation will cover.
struct BarContent {
    double fillFrom{0.0}, fillTo{0.0};  ///< normalised, in either order
    double modFrom{0.0}, modTo{0.0};    ///< normalised modulation reach; equal means none
    bool tick{false};                   ///< mark where a signed value's zero is
    bool enabled{true};
    juce::Colour ink{theme::colour::valueFill};
    double live{0.0};     ///< normalised; where the engine has it now
    bool hasLive{false};  ///< it differs from the set value: mark it
};
void drawValueBar(juce::Graphics& g, juce::Rectangle<float> area, const BarContent& bar);

/// When another instance is selected: what is missing, and a way back.
void drawRemoteNotice(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label);
juce::Rectangle<float> remoteNoticeLink(juce::Rectangle<float> area);

/// A std::string from the core, as a juce::String.
juce::String fromCore(std::string_view s);

/// A source's name as a tab shows it: "lfo 1", "sc attack". The manifest says whose sources.
juce::String sourceLabel(const ModManifest& m, int slot);

/// A toggle in the scene's corner: a small square and a lowercase label. A press on `hit` must never start an orbit.
struct SceneToggle {
    juce::Rectangle<float> hit;  ///< the square and the label: a label you cannot click looks broken
    float left;                  ///< where it starts, so a second toggle sits to its left
};
SceneToggle paintSceneToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge,
                             const juce::String& label, bool on, bool enabled = true);
/// How wide one is, square and label: what a caller laying several out from the left needs to know.
float sceneToggleWidth(const juce::String& label);

/// Where a scene view draws, in its own pixels: label strip at the top, picture below. The equirect
/// is 2:1 and nothing else; `equirectWidth`/`equirectHeight` cap the largest 2:1 box drawn inside both.
Viewport sceneViewport(Projection projection, int width, int height);

/// Where the two scene panels go in the row they share. `full` gives the whole row to the equirect and hides the globe.
struct SceneRow {
    juce::Rectangle<int> globe;  ///< empty when the equirect has the row
    juce::Rectangle<int> equirect;
};
SceneRow sceneRow(int left, int top, bool full);

/// The pieces a page is made of, drawn. They draw and say where; whoever calls them registers the presses.

/*  One option of a choice -- a tab, a sub-tab, a segment: its name in black, and when chosen a black
    block with the name in light text. Grey is only for an option that cannot be chosen. */
void drawChoice(juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& label, const juce::Font& f,
                bool chosen, bool offered = true);
/// A choice as segments: equal cells, one `drawChoice` each. Returns each segment's cell.
/// `unavailable` is one drawn greyed that the state cannot reach, or -1.
std::vector<juce::Rectangle<float>> drawSegments(juce::Graphics& g, juce::Rectangle<float> area,
                                                 const std::vector<juce::String>& names, int selected, bool enabled,
                                                 int unavailable = -1);
/// The same row wrapped: `columns` cells a line, the lines touching, so the cells stand in one grid.
/// `columns` <= 0 or >= the count draws one line.
std::vector<juce::Rectangle<float>> drawSegmentLines(juce::Graphics& g, float x, float y, float width,
                                                     const std::vector<juce::String>& names, int selected, bool enabled,
                                                     int unavailable, int columns);
float segmentLinesHeight(int count, int columns);
/// An action: framed, its label centred, greyed while disabled.
void drawFramedButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, bool enabled);
/// How wide `drawFramedButton` wants to be for `label`.
float framedButtonWidth(const juce::String& label);
/// A chevron in its box. `dx`/`dy` are the direction it points -- (1, 0) right, (0, 1) down -- one of them zero.
void strokeChevron(juce::Graphics& g, juce::Rectangle<float> box, int dx, int dy, juce::Colour c);
/// A group's name with a rule under it, `width` wide from `x`. Returns the y to carry on at.
float drawGroupHeading(juce::Graphics& g, float x, float y, float width, const juce::String& title);
/// A fact: its name on the left and what it is on the right, a divider under it. `set` false greys the value.
void drawFactRow(juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, const juce::String& value,
                 bool set = true);
/// The cross that closes something temporary, in `box`. Returns the area a press on it should take.
juce::Rectangle<float> drawCloseCross(juce::Graphics& g, juce::Rectangle<float> box);

/// The grid both source graphs draw: nice ticks with zero emphasised, relabelled by polarity and
/// never moving the drawing under them. `plot` is the drawing itself; values arrive already
/// normalised to the curve's own [0, 1]. Returns the row the zero tick landed on, or nothing.
std::optional<float> drawNiceGrid(juce::Graphics& g, juce::Rectangle<float> plot, double polarity, int maxTicks);

}  // namespace bambi::ui
