// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/SceneStrip.h"

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

SceneStripAreas paintSceneStrip(juce::Graphics& g, int width, Projection projection, const SceneViewState& view) {
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
        return areas;
    }

    /*  On the EQUIRECT, and in one place rather than one a view: two spheres each carrying the same
        switches would be two controls for one setting, and the settings are the window's. */
    const juce::Rectangle<float> row{0.0f, 0.0f, wide, strip};
    auto right = wide - static_cast<float>(sc::labelInset);
    const auto regions = paintSceneToggle(g, row, right, "regions always", view.regionsAlways);
    areas.regions = regions.hit;
    right = regions.left - sc::toggleGroupGap;
    /*  The visualiser switch is greyed while the window shows another instance: a field's energy
        never crosses the bus, so there is nothing to switch. */
    const auto energy =
        paintSceneToggle(g, row, right, "energy", view.energyOn && view.energyAvailable, view.energyAvailable);
    areas.energy = energy.hit;
    right = energy.left - sc::toggleGroupGap;
    //  the whole row, with the globe hidden: the same sphere, four times the size
    areas.full = paintSceneToggle(g, row, right, "full", view.equirectFull).hit;
    return areas;
}

void addSceneStripClicks(HitArea& on, const SceneStripAreas& areas, SceneViewState& view,
                         std::function<void()> relayout, std::function<void()> changed) {
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
}

}  // namespace bambi::ui
