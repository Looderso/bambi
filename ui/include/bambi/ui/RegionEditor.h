// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/RegionOverlay.h"

/// The region editor: one component, the encoder's region source tab and an effect's slot tabs alike.
namespace bambi::ui {

/// What a page tells the editor about the slot it is drawing.
struct RegionSlot {
    int index{0};                   ///< which region slot: 0 is the encoder's, or an effect's send
    std::string prefix{"region1"};  ///< its parameter block's key prefix
    std::string role;               ///< "send", "return": what a matrix row is named after
};

inline constexpr std::array<const char*, 7> kRegionKindNames{"everywhere", "spot",   "band",  "sectors",
                                                             "dots",       "clouds", "custom"};
inline constexpr int kRegionCustomEntry = 6;

/// Draws the editor's two sub-tabs, shape and transform, and returns the y below them.
float paintRegionTabs(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y);

/// Draws the open sub-tab of the editor for `slot` into `content` from `y`, and returns the y it ended at.
float paintRegionEditor(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                        const RegionSlot& slot);

/// Which region angle a parameter is, if it is one: `region2.pitch` is slot 1, axis 1.
bool regionAngleOf(const ParamManifest& m, ParamId id, int& slot, int& axis);

/// The region a slot resolves to: shape from state, settings from the parameters, turn from the caller.
bambi::Region regionOf(const PatchModel& model, const RegionSlot& slot, double yawTurnRad = 0.0,
                       double pitchTurnRad = 0.0, double rollTurnRad = 0.0);

/// A handle carried to `to`, written back through the slot's parameters. `now` is the region as
/// drawn, turn included, so a dragged edge measures from where it looks.
void carryRegionHandle(PatchModel& model, const RegionSlot& slot, const bambi::Region& now, RegionHandle::Kind kind,
                       Vec3 to, double yawTurnRad = 0.0, double pitchTurnRad = 0.0, double rollTurnRad = 0.0);

/// A handle double-clicked back to its default: aim to front, roll to zero, an edge or elevation to
/// the parameter's own default.
void resetRegionHandle(PatchModel& model, const RegionSlot& slot, const bambi::Region& now, RegionHandle::Kind kind);

}  // namespace bambi::ui
