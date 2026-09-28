// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <juce_graphics/juce_graphics.h>

#include "bambi/scene/energy.hpp"
#include "bambi/scene/view.hpp"

/// The strip over a scene view: its caption and, beside it, what belongs to the view as a whole --
/// the view presets on the globe, `full`/`energy`/`regions always` on the equirect. It paints and
/// says where; a view registers the clicks, added last so they lie on top.
namespace bambi::ui {

inline constexpr std::array<ViewPreset, 3> kViewPresets{ViewPreset::Top, ViewPreset::Front, ViewPreset::Side};
inline constexpr std::array<const char*, 3> kViewPresetNames{"top", "front", "side"};

class HitArea;

/// How a window's scene is looked at: the globe's camera and preset, and the two switches of the
/// scene's corner. Editor state, not the patch's -- none of it changes a sound. Shared by the
/// encoder's scene state and an effect's probe state.
struct SceneViewState {
    Camera camera;                        ///< the globe's viewpoint, orbited by a drag
    ViewPreset preset{ViewPreset::Free};  ///< which of the strip's presets it is on; Free once a drag orbits it
    bool regionsAlways{true};             ///< off shows a region only while its page is open
    bool equirectFull{false};             ///< the equirect has the whole scene row and the globe is hidden

    /// Off when a window opens, and not remembered: the one thing here that costs, so with several
    /// windows open in a host none draws it unless asked to; while off, the audio thread gathers nothing for it.
    bool energyOn{false};
    bool energyAvailable{
        true};  ///< false while the window shows another instance: its energy never crosses the link bus
    EnergyField energy;
};

/// A drag on the globe: the camera turns, and whichever preset it was on is no longer where it is.
void orbitView(SceneViewState& view, double dx, double dy);

/// Where each control was drawn; empty where this projection has none.
struct SceneStripAreas {
    std::array<juce::Rectangle<float>, 3> presets{};
    juce::Rectangle<float> regions, energy, full;
};

SceneStripAreas paintSceneStrip(juce::Graphics& g, int width, Projection projection, const SceneViewState& view);

/// The strip's controls take their clicks. Called last in a view's paint so they lie on top and a
/// press on one never orbits the globe. `relayout` runs when `full` flips, before `changed`.
void addSceneStripClicks(HitArea& on, const SceneStripAreas& areas, SceneViewState& view,
                         std::function<void()> relayout, std::function<void()> changed);

}  // namespace bambi::ui
