// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <span>
#include <vector>

#include "bambi/scene/overlay.hpp"
#include "bambi/ui/HitArea.h"

namespace bambi::ui {

/*  The scene's overlay toggle with a list behind it: a small triangle beside a corner toggle opens one row for each thing
 *  the toggle governs -- every instance on the bus, and an instance's parts where it has any -- each with its own box.
 *  The toggle stays the master: off hides everything and keeps the boxes as they were. Ticking a row while the master is
 *  off changes the row and leaves the master alone, and the rows are greyed to say nothing is drawn.
 *
 *  Written once for every scene in every plugin. It draws, says where it drew, and registers the presses; what is drawn
 *  for a row is the caller's, who asks `OverlaySelection::drawn`. The state here is the window's own, never a parameter.
 */

/// One line of the list: an item, or a part of the item above it.
struct OverlayRow {
    OverlayKey key;
    juce::String label;
    juce::Colour swatch;  ///< the dot the scene draws the item with; a part has none
    bool part{false};     ///< a part of the item above it: indented, with no dot of its own
};

/// What the window keeps of the list while it is looked at.
struct OverlayMenuState {
    bool open{false};
    float scroll{0.0f};                 ///< pixels the list is scrolled by
    std::optional<OverlayKey> hovered;  ///< the row under the pointer, so the scene can point at what it names
};

struct OverlayToggleAreas {
    juce::Rectangle<float> toggle;    ///< the square and the label
    juce::Rectangle<float> triangle;  ///< the glyph that opens the list
    float left{0.0f};                 ///< where the pair starts, so a second toggle sits to its left
};

/// Where the list was drawn. `rows` is parallel to the rows given; a row scrolled out of sight has an empty box.
struct OverlayMenuAreas {
    juce::Rectangle<float> box, all, none;
    std::vector<juce::Rectangle<float>> rows;
    float maxScroll{0.0f};
};

/// The toggle and its triangle, from `rightEdge` to the left. `listed` false -- nothing on the bus -- greys both and
/// takes no press.
OverlayToggleAreas paintOverlayToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge,
                                      const juce::String& label, const OverlaySelection& selection, bool listed,
                                      const OverlayMenuState& state);

/// The list, below `top`, its right edge at `rightEdge`. Nothing is drawn while it is closed. Draw it last, over the scene.
OverlayMenuAreas paintOverlayMenu(juce::Graphics& g, float rightEdge, float top, std::span<const OverlayRow> rows,
                                  const OverlaySelection& selection, const OverlayMenuState& state);

/// The row under `at`, if any. For the pointer's move over the scene.
std::optional<OverlayKey> overlayRowAt(const OverlayMenuAreas& areas, std::span<const OverlayRow> rows,
                                       juce::Point<float> at);

/// The pointer moved over the scene: the row it is on becomes the hovered one, so the scene can point at its item. True when that
/// changed and the scene wants a repaint. The pointer's move is the view's to receive; what to do with it is here.
bool overlayHover(const OverlayMenuAreas& areas, std::span<const OverlayRow> rows, OverlayMenuState& state,
                  juce::Point<float> at);

/*  The presses: the master, the triangle and, while open, the list -- a row's box, all and none, the wheel -- and one
 *  over `whole` that closes the list on a press and swallows it, so the press that closes it never starts an orbit.
 *  Called after the view's own areas, since later ones lie on top. `changed` repaints. */
void addOverlayClicks(HitArea& on, const OverlayToggleAreas& toggle, const OverlayMenuAreas& menu,
                      std::span<const OverlayRow> rows, OverlaySelection& selection, OverlayMenuState& state,
                      juce::Rectangle<float> whole, std::function<void()> changed);

/*  One overlay's menu, whole: its choices, whether its list is open, the rows it names and where it was last drawn. A scene owns
 *  one per overlay and does four things with it, in its own paint and its own events -- nothing else, so no scene writes the
 *  sequence again:
 *
 *      each frame      setRows(...)                      what is on the bus to name
 *      paint           paintToggle(...)  ...  paintList(...)  the list last, over everything
 *      after the view's own areas   addClicks(...)
 *      the view's events            mouseMove, mouseExit, keyPressed
 *
 *  The items it draws ask `drawn(key)`; a scene that draws the overlay twice -- a globe and an equirect -- shares the one menu
 *  and gives the toggle to one of them.
 */
class OverlayMenu {
public:
    /// `defaultOn`: whether an item without a choice of its own is ticked. `masterOn`: whether the overlay is drawn at all until
    /// the toggle is pressed.
    explicit OverlayMenu(juce::String label, bool defaultOn = true, bool masterOn = true)
        : label_(std::move(label)), selection_(defaultOn) {
        selection_.setMaster(masterOn);
    }

    /// The rows the list names now. A choice for something no longer among them is forgotten, so the choices of instances that
    /// left never accumulate.
    void setRows(std::vector<OverlayRow> rows);
    std::span<const OverlayRow> rows() const { return rows_; }

    bool drawn(const OverlayKey& key) const { return selection_.drawn(key); }
    bool hovered(const OverlayKey& key) const { return state_.hovered.has_value() && *state_.hovered == key; }
    bool open() const { return state_.open; }
    OverlaySelection& selection() { return selection_; }
    const OverlaySelection& selection() const { return selection_; }

    /// The toggle and its triangle in `strip`, from `rightEdge`; remembers where the list hangs. Returns where the pair starts.
    float paintToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge);
    /// The list, below `top`; nothing while closed. Last in the view's paint.
    void paintList(juce::Graphics& g, float top);
    /// The presses. After the view's own areas, which the list must lie on top of. `whole` is the view's bounds.
    void addClicks(HitArea& on, juce::Rectangle<float> whole, std::function<void()> changed);

    /// The pointer moved over the view, left it, or a key was pressed with the view focused. Each returns true when the view
    /// should repaint; `keyPressed` also when it took the key (Escape closes an open list).
    bool mouseMove(juce::Point<float> at);
    bool mouseExit();
    bool keyPressed(const juce::KeyPress& key);

    const OverlayToggleAreas& toggleAreas() const { return toggle_; }
    const OverlayMenuAreas& listAreas() const { return list_; }

private:
    juce::String label_;
    OverlaySelection selection_;
    OverlayMenuState state_;
    std::vector<OverlayRow> rows_;
    OverlayToggleAreas toggle_;
    OverlayMenuAreas list_;
    float right_{0.0f};
};

}  // namespace bambi::ui
