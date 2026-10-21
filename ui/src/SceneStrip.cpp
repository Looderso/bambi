// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/SceneStrip.h"

#include "bambi/ui/DecoderOverlay.h"
#include "bambi/ui/Draw.h"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace sc = theme::scene;
}  // namespace

void orbitView(SceneViewState& view, double dx, double dy) {
    orbit(view.camera, dx, dy);
    view.preset = ViewPreset::Free;
}

void refreshSceneOverlays(SceneViewState& view, const LinkScene& scene) {
    view.linkScene = &scene;
    view.decoders.setRows(decoderRows(scene));
}

SceneStripAreas paintSceneStrip(juce::Graphics& g, int width, Projection projection, SceneViewState& view,
                                bool withRegions, bool withDecoders) {
    SceneStripAreas areas;
    const bool globe = projection == Projection::Globe;
    const auto strip = static_cast<float>(sc::labelStrip);
    const auto wide = static_cast<float>(width);

    text(g, globe ? "globe" : "equirect", labelFont(), colour::label,
         {static_cast<float>(sc::labelInset), 0.0f, wide / 2.0f, strip});

    if (globe) {
        //  right-aligned at the panel's inset, laid out from the right so the last sits at the edge
        const auto f = labelFont();
        const auto padX = static_cast<float>(sc::presetChipPadX);
        const auto height = f.getHeight() + 2.0f * static_cast<float>(sc::presetChipPadY);
        const auto top = (strip - height) / 2.0f;
        auto right = wide - static_cast<float>(sc::labelInset);
        for (std::size_t i = kViewPresets.size(); i-- > 0;) {
            const auto chipWidth = textWidth(f, kViewPresetNames[i]) + 2.0f * padX;
            right -= chipWidth;
            areas.presets[i] = {right, top, chipWidth, height};
        }
        for (std::size_t i = 0; i < kViewPresets.size(); ++i)
            drawChoice(g, areas.presets[i], kViewPresetNames[i], f, view.preset == kViewPresets[i]);
        //  the decoder overlay beside the presets, unless the equirect has the row and carries it instead
        if (withDecoders && !view.equirectFull) {
            const juce::Rectangle<float> row{0.0f, 0.0f, wide, strip};
            view.decoders.paintToggle(g, row, areas.presets[0].getX() - sc::toggleGroupGap);
            areas.decodersHere = true;
        }
        return areas;
    }

    /*  On the EQUIRECT, and in one place rather than one a view: two spheres each carrying the same
        switches would be two controls for one setting, and the settings are the window's. */
    const juce::Rectangle<float> row{0.0f, 0.0f, wide, strip};
    auto right = wide - static_cast<float>(sc::labelInset);
    if (withRegions) {
        const auto regions = paintSceneToggle(g, row, right, "regions always", view.regionsAlways);
        areas.regions = regions.hit;
        right = regions.left - sc::toggleGroupGap;
    }
    /*  The visualiser switch is greyed while the window shows another instance: a field's energy
        never crosses the bus, so there is nothing to switch. */
    const auto energy =
        paintSceneToggle(g, row, right, "energy", view.energyOn && view.energyAvailable, view.energyAvailable);
    areas.energy = energy.hit;
    right = energy.left - sc::toggleGroupGap;
    //  the whole row, with the globe hidden: the same sphere, four times the size
    const auto full = paintSceneToggle(g, row, right, "full", view.equirectFull);
    areas.full = full.hit;
    if (withDecoders && view.equirectFull) {
        view.decoders.paintToggle(g, row, full.left - sc::toggleGroupGap);
        areas.decodersHere = true;
    }
    return areas;
}

void addSceneStripClicks(HitArea& on, const SceneStripAreas& areas, SceneViewState& view,
                         const std::function<void()>& relayout, const std::function<void()>& changed) {
    const auto notify = [changed] {
        if (changed) changed();
    };
    for (std::size_t i = 0; i < areas.presets.size(); ++i)
        if (!areas.presets[i].isEmpty())
            on.addClickArea(areas.presets[i], [&view, i, notify] {
                view.preset = kViewPresets[i];
                view.camera = cameraFor(kViewPresets[i]);
                notify();
            });
    if (!areas.regions.isEmpty())
        on.addClickArea(areas.regions, [&view, notify] {
            view.regionsAlways = !view.regionsAlways;
            notify();
        });
    if (!areas.energy.isEmpty() && view.energyAvailable)  // greyed takes no click
        on.addClickArea(areas.energy, [&view, notify] {
            view.energyOn = !view.energyOn;
            notify();
        });
    if (!areas.full.isEmpty())
        on.addClickArea(areas.full, [&view, relayout, notify] {
            view.equirectFull = !view.equirectFull;
            if (relayout) relayout();
            notify();
        });
    //  the decoder list over everything this view has registered, its presses first to be taken
    if (areas.decodersHere) view.decoders.addClicks(on, on.getLocalBounds().toFloat(), notify);
}

DecoderOverlayDrawn paintSceneOverlays(juce::Graphics& g, Projection projection, const Viewport& vp,
                                       const SceneViewState& view) {
    if (view.linkScene == nullptr || !view.decoders.selection().master()) return {};
    return paintDecoderOverlay(g, projection, view.camera, vp, *view.linkScene, view.decoders);
}

void paintSceneStripList(juce::Graphics& g, const SceneStripAreas& areas, SceneViewState& view) {
    if (areas.decodersHere) view.decoders.paintList(g, static_cast<float>(sc::labelStrip));
}

bool sceneStripPointer(SceneViewState& view, const SceneStripAreas& areas, std::optional<juce::Point<float>> at) {
    if (!areas.decodersHere) return false;
    return at.has_value() ? view.decoders.mouseMove(*at) : view.decoders.mouseExit();
}

bool sceneStripKey(SceneViewState& view, const SceneStripAreas& areas, const juce::KeyPress& key) {
    return areas.decodersHere && view.decoders.keyPressed(key);
}

}  // namespace bambi::ui
