// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panel.h"

#include <algorithm>

#include "bambi/reverb/control.hpp"
#include "bambi/ui/SourceSettings.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

constexpr const char* kTabNames[] = {"room", "send", "return"};
}  // namespace

ReverbPanel::ReverbPanel(ReverbControlState& state) : ParameterPage(state), state_(state) { setOpaque(true); }

void ReverbPanel::paint(juce::Graphics& g) {
    clearRegions();
    liveRegion() = {};
    clearNameAreas();
    rooms_ = {};

    //  the bar, the rule, the viewport, the remote notice and the temporary source tab: the page's
    const auto frame = paintTabbedFrame(g, {kTabNames[0], kTabNames[1], kTabNames[2]});
    if (frame.content.isEmpty()) return;

    const ContentClip clip(*this, g);  // the tab bar stays put; what it governs scrolls under it
    switch (state_.tab) {
        case 1:
        case 2: paintRegions(g, frame.content, state_.tab - 1); break;
        case 3: {
            const int slot = state_.editedRegion();
            paintSourceSettings(*this, g, frame.content, state_.source,
                                {slot, slot == 0 ? "region1" : "region2", slot == 0 ? "send" : "return"});
            break;
        }
        default: paintRoom(g, frame.content); break;
    }
}

/*  One row of starting points, tuned by ear. A room sets the room's parameters and its cuts and
    leaves the levels, the regions and the quality alone -- which `applyPreset` decides, not this
    panel: the keys a room touches are the same fact as the room. The shape rides with the
    selection, in state, because it is the room's and not a control. The chosen room stays lit
    while its values are edited; a double-click brings its values back. */
float ReverbPanel::paintRooms(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    std::vector<juce::String> names;  // no title: the tab is the room's already
    for (const auto n : bambi::kRoomNames) names.push_back(fromCore(n));
    const auto apply = [this](int which) {
        /*  Only the keys the room touches, through the host's own parameters and inside gestures,
            so it automates and undoes exactly as turning those knobs by hand would. Writing the
            whole list back would undo edits the editor's copy of the patch has not caught up
            with yet. The selection and the shape are one undoable edit. */
        bambi::RoomState room;
        bambi::applyPreset(
            model().manifest(), static_cast<bambi::ReverbPreset>(which),
            [this](int at, float normalised) { model().changeParameter(static_cast<bambi::ParamId>(at), normalised); },
            room);
        model().applyEdit("room", [room](bambi::PluginState& s) { s.room = room; });
        model().notifyChanged();
    };
    const auto row = paintPicker(g, content.getX(), y, content.getWidth(), names,
                                 std::clamp(state_.patch.room.preset, 0, 5), true, apply, apply, -1, 3);
    for (std::size_t i = 0; i < rooms_.size() && i < row.cells.size(); ++i) rooms_[i] = row.cells[i];
    return row.bottom + static_cast<float>(theme::space::groupGap);
}

/*  Everything about the engine and its I/O, the same shape as Echo's `taps` tab. The balance of
    reflections and tail is not here, because it is not a control: it follows from the room, and
    set by hand it sounded off every time. */
void ReverbPanel::paintRoom(juce::Graphics& g, juce::Rectangle<float> content) {
    const auto id = [this](const char* key) { return state_.param(key); };
    auto y = paintRooms(g, content, content.getY());
    //  no shape tile: the shape is the room's and came with the row
    y = paintGroup(g, content, y, {},
                   {id("room.size"), id("room.decay"), id("room.tone"), id("room.roughness"), id("room.distance"),
                    id("output.pre_delay")});
    /*  Not here: wet and dry, and the quality. They are the whole instance's and not the room's,
        so they are always on screen -- the levels in the column down the right edge, the quality
        in the footer. The pre-delay is the room's, and stays with it. */
    paintGroup(g, content, y, "input", {id("input.low_cut"), id("input.high_cut")});
}

/*  Send and return are two tabs, as Echo's send is one. The editor is the shared one. */
void ReverbPanel::paintRegions(juce::Graphics& g, juce::Rectangle<float> content, int slot) {
    const auto y = paintRegionTabs(*this, g, content, content.getY());
    paintRegionEditor(*this, g, content, y, {slot, slot == 0 ? "region1" : "region2", slot == 0 ? "send" : "return"});
}

void ReverbPanel::showSlot(int which) {
    state_.showTab(1 + std::clamp(which, 0, 1));  // a slot is its tab now
}

juce::Rectangle<float> ReverbPanel::roomButton(int index) const {
    return index >= 0 && index < 6 ? rooms_[static_cast<std::size_t>(index)] : juce::Rectangle<float>{};
}

bool ReverbPanel::touchParameter(bambi::ParamId id) {
    const auto area = nameArea(id);
    if (area.isEmpty()) return false;
    clickAt(area.getCentre());
    return true;
}

}  // namespace bambi::ui
