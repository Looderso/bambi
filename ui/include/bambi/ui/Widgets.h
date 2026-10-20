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
/// The box the picture fills in a scene view: the globe's circle is inscribed in it, the equirect is it.
juce::Rectangle<float> sceneFrame(const Viewport& vp);
/// The globe's outline, or the equirect's frame, in the sphere's colour.
void paintSceneFrame(juce::Graphics& g, const Viewport& vp, Projection projection);

/// Where the two scene panels go in the row they share. `full` gives the whole row to the equirect and hides the globe.
struct SceneRow {
    juce::Rectangle<int> globe;  ///< empty when the equirect has the row
    juce::Rectangle<int> equirect;
};
SceneRow sceneRow(int left, int top, bool full);

/// The pieces a page is made of, drawn. They draw and say where; whoever calls them registers the presses.

/*  One option of a choice -- a tab, a sub-tab, a segment: its name in black, and when chosen a black
    block with the name in light text. Grey is only for an option that cannot be chosen: `offered` false
    greys the name, and a chosen one is then a grey block. */
void drawChoice(juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& label, const juce::Font& f,
                bool chosen, bool offered = true);
/// A choice as segments: equal cells, one `drawChoice` each. Returns each segment's cell.
/// `unavailable` is the first of the segments drawn greyed that the state cannot reach -- it and every one after
/// it -- or -1.
std::vector<juce::Rectangle<float>> drawSegments(juce::Graphics& g, juce::Rectangle<float> area,
                                                 const std::vector<juce::String>& names, int selected, bool enabled,
                                                 int unavailable = -1);
/// How wide `drawSegments` wants to be for `names`: as many equal cells as names, each fitting the widest.
float segmentsWidth(const std::vector<juce::String>& names);
/// The same row wrapped: `columns` cells a line, the lines touching, so the cells stand in one grid.
/// `columns` <= 0 or >= the count draws one line.
std::vector<juce::Rectangle<float>> drawSegmentLines(juce::Graphics& g, float x, float y, float width,
                                                     const std::vector<juce::String>& names, int selected, bool enabled,
                                                     int unavailable, int columns);
float segmentLinesHeight(int count, int columns);
/// An action: framed, its label centred, greyed while disabled.
/// `on` draws it as a chosen block: an action that is running, such as a test burst, until clicked again.
void drawFramedButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, bool enabled,
                      bool on = false);
/// How wide `drawFramedButton` wants to be for `label`.
float framedButtonWidth(const juce::String& label);
/*  A dropdown: the small triangle, pointing down, that opens a list beside what it belongs to -- the scene's overlay
    toggle, a chip on the live host's routing page. The one look for a list behind a control, everywhere. `box` is
    what takes the press, its height the control's; while the list is open the triangle sits on a block, as an open
    tab does, `blockHeight` tall; greyed when it offers nothing. */
void drawDropdown(juce::Graphics& g, juce::Rectangle<float> box, bool open, bool offered, float blockHeight);
/// How wide a dropdown's box is: the triangle and its pad either side.
float dropdownWidth();
/// A chevron in its box. `dx`/`dy` are the direction it points -- (1, 0) right, (0, 1) down -- one of them zero.
void strokeChevron(juce::Graphics& g, juce::Rectangle<float> box, int dx, int dy, juce::Colour c);
/// A group's name with a rule under it, `width` wide from `x`. Returns the y to carry on at.
float drawGroupHeading(juce::Graphics& g, float x, float y, float width, const juce::String& title);
/// A fact: its name on the left and what it is on the right, a divider under it. `set` false greys the value.
void drawFactRow(juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, const juce::String& value,
                 bool set = true);
/// Something waiting for the user's next act -- learn listening for a note, the live host's sidechain slot waiting
/// for a source: a block in the learning colour, its label in the ground's.
void drawWaiting(juce::Graphics& g, juce::Rectangle<float> box, const juce::String& label, const juce::Font& f);
/// What a waiting pick may take: a dashed ring in the waiting colour round `box`.
void drawTakes(juce::Graphics& g, juce::Rectangle<float> box);
/// What a waiting pick cannot take: the ground laid thinly over `box`, so it reads as out of reach.
void drawOutOfReach(juce::Graphics& g, juce::Rectangle<float> box);
/*  A chip: a small framed label, a thing offered or held for a click -- a preset's folder, an insert on the live host's
    page. `accent`, when given, is a bar on its left in its owner's colour; `closable` puts a cross at its right,
    whose area is returned. */
struct Chip {
    juce::Rectangle<float> box;
    juce::Rectangle<float> cross;  ///< empty when not closable
};
Chip drawChip(juce::Graphics& g, juce::Point<float> topLeft, float height, const juce::String& label, juce::Colour ink,
              std::optional<juce::Colour> accent = std::nullopt, bool closable = false);
/// How wide `drawChip` draws `label`, at `height`, with a bar and a cross or without.
float chipWidth(const juce::String& label, float height, bool accent, bool closable);
/// Whether an output plays, as the Decoder's tabs mark it: a light well, a dark square in it while it plays.
/// `onBlock` draws its frame light, for a mark on a chosen block.
void drawPlayMark(juce::Graphics& g, juce::Rectangle<float> box, bool plays, bool onBlock = false);

/*  A menu: a list dropped from `topLeft`, framed, an item a line -- its name, and a note right of it -- and an
    explanation under them all. An item not `offered` is greyed and is to take no press. Whoever draws it registers
    the presses on the items it offers, and closes it on a press anywhere else. */
struct MenuItem {
    juce::String label, note;
    bool offered{true};
};
struct Menu {
    juce::Rectangle<float> box;
    std::vector<juce::Rectangle<float>> items;
};
Menu drawMenu(juce::Graphics& g, juce::Point<float> topLeft, float width, const std::vector<MenuItem>& items,
              const juce::String& foot);
/// The panel every menu stands on: the ground, outlined in the strong rule. `drawMenu` and the overlay list both start here.
void paintMenuFrame(juce::Graphics& g, juce::Rectangle<float> box);

/// A live host page's band, along its top: the ground, a rule under it. Returns the room inside its insets.
juce::Rectangle<float> drawPageBand(juce::Graphics& g, float width);

/// The cross that closes something temporary, in `box`. Returns the area a press on it should take.
juce::Rectangle<float> drawCloseCross(juce::Graphics& g, juce::Rectangle<float> box);

/// The grid both source graphs draw: nice ticks with zero emphasised, relabelled by polarity and
/// never moving the drawing under them. `plot` is the drawing itself; values arrive already
/// normalised to the curve's own [0, 1]. Returns the row the zero tick landed on, or nothing.
std::optional<float> drawNiceGrid(juce::Graphics& g, juce::Rectangle<float> plot, double polarity, int maxTicks);

}  // namespace bambi::ui
