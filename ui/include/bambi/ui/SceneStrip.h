// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <juce_graphics/juce_graphics.h>
#include <optional>

#include "bambi/link/link.hpp"
#include "bambi/scene/energy.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/DecoderOverlay.h"
#include "bambi/ui/OverlayMenu.h"

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

    /*  The session's decoders, drawn in the scene while this is on: their loudspeakers and the listener's head, as the
        Decoder plays them. Off when a window opens and not remembered, like the energy picture; once on, every decoder and
        both its parts are ticked until the list says otherwise. A view's `linkScene` is the session's instances as the
        window last read them. */
    OverlayMenu decoders{"decoder", true, false};
    const LinkScene* linkScene{nullptr};
};

/// The window has read the session again: the decoder overlay names what is on the bus now.
void refreshSceneOverlays(SceneViewState& view, const LinkScene& scene);

/// A drag on the globe: the camera turns, and whichever preset it was on is no longer where it is.
void orbitView(SceneViewState& view, double dx, double dy);

/// Where each control was drawn; empty where this projection has none.
struct SceneStripAreas {
    std::array<juce::Rectangle<float>, 3> presets{};
    juce::Rectangle<float> regions, energy, full;
    bool decodersHere{false};  ///< this view carries the `decoder` toggle, and so draws its list and takes its presses
};

/// `withRegions` false leaves the `regions always` toggle out, for a plugin that has no regions to show on its scene.
/// `withDecoders` adds the `decoder` toggle and its triangle: on the globe, or on the equirect while it has the whole row.
SceneStripAreas paintSceneStrip(juce::Graphics& g, int width, Projection projection, SceneViewState& view,
                                bool withRegions = true, bool withDecoders = false);

/// The session's decoders, drawn in the room's frame: in a view's paint, over the graticule and under everything of its own.
DecoderOverlayDrawn paintSceneOverlays(juce::Graphics& g, Projection projection, const Viewport& vp,
                                       const SceneViewState& view);
/// The `decoder` list, if this view carries its toggle: the last thing a view draws.
void paintSceneStripList(juce::Graphics& g, const SceneStripAreas& areas, SceneViewState& view);
/// A view's pointer moved to `at`, or left (no `at`), or a key was pressed: what the list does with them. True when the view
/// should repaint, and for a key when the list took it.
bool sceneStripPointer(SceneViewState& view, const SceneStripAreas& areas, std::optional<juce::Point<float>> at);
bool sceneStripKey(SceneViewState& view, const SceneStripAreas& areas, const juce::KeyPress& key);

/// The strip's controls take their clicks. Called last in a view's paint so they lie on top and a
/// press on one never orbits the globe. `relayout` runs when `full` flips, before `changed`.
void addSceneStripClicks(HitArea& on, const SceneStripAreas& areas, SceneViewState& view,
                         std::function<void()> relayout, std::function<void()> changed);

}  // namespace bambi::ui
