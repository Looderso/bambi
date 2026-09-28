// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/editor/EffectEditor.h"

#include <algorithm>

#include "bambi/editor/EffectFrame.h"
#include "bambi/region/shape.hpp"

namespace bambi::editor {

EffectViews::EffectViews(EffectControls& controls)
    : globe(
          controls.sphere, Projection::Globe,
          [&controls](Vec3 from, ui::ProbeReply& into) { controls.answerProbe(from, into); },
          [&controls] { controls.notify(); }),
      equirect(
          controls.sphere, Projection::Equirect,
          [&controls](Vec3 from, ui::ProbeReply& into) { controls.answerProbe(from, into); },
          [&controls] { controls.notify(); }) {
    ui::RegionHook hook;
    hook.regions = [&controls](std::vector<ui::SceneRegion>& into) { controls.regionsForScene(into); };
    hook.carried = [&controls](ui::RegionHandle::Kind kind, Vec3 to) { controls.carryRegion(kind, to); };
    hook.reset = [&controls](ui::RegionHandle::Kind kind) { controls.resetRegion(kind); };
    globe.setRegionHook(hook);
    equirect.setRegionHook(hook);
}

EffectEditor::EffectEditor(host::FieldEffectProcessor& processor, EffectControls& controls, ui::ParameterPage& panel,
                           const juce::String& plugin, std::vector<ui::LevelColumn::Level> levels,
                           std::vector<ui::StatusFooter::Switch> footerSwitches)
    : EffectViews(controls),
      PluginEditor(processor, {controls, controls, panel, globe, equirect, controls.sphere}, plugin, std::move(levels),
                   std::move(footerSwitches)),
      effect_(processor),
      effectControls_(controls) {
    equirect.setRelayout([this] { relayout(); });
    /*  A first frame before the first tick, as the encoder's window has: without it the window's
        first paint has no header tabs, no footer readout and no undo state. `frame` is FINAL in this
        class, so calling it from this constructor reaches this class's own and nothing unbuilt. */
    tick();
}

void EffectEditor::frame(ui::EditorFrame& parts) {
    EffectFrame{effectControls_, globe, equirect, parts}.tick(
        effect_.linkStatus(), effect_.diagnostics().outputPeak.load(std::memory_order_relaxed), effect_.engineOrder(),
        effect_.dspLoad());
}

void EffectEditor::controlsChanged() {
    globe.repaint();
    equirect.repaint();
}

void EffectEditor::addOwnSettings(ui::SettingsContent& c) { addEffectSettings(c); }

bool EffectEditor::placeProbe(double nx, double ny, bool onGlobe) {
    paintNow();
    return (onGlobe ? globe : equirect).placeAt(nx, ny);
}

void EffectEditor::clickProbe(double nx, double ny, bool onGlobe) {
    paintNow();
    auto& view = onGlobe ? globe : equirect;
    view.clickAt(view.pixelsFor(nx, ny));
}

void EffectEditor::dragProbe(double fromNx, double fromNy, double toNx, double toNy, bool onGlobe) {
    paintNow();
    auto& view = onGlobe ? globe : equirect;
    view.dragBetween(view.pixelsFor(fromNx, fromNy), view.pixelsFor(toNx, toNy));
}

bool EffectEditor::clickViewPreset(int index) {
    paintNow();
    const auto box = globe.presetArea(index);
    if (box.isEmpty()) return false;
    globe.clickAt(box.getCentre());
    return true;
}

bool EffectEditor::cameraMoved(Camera before) const {
    return !juce::exactlyEqual(before.yaw, effectControls_.sphere.camera.yaw) ||
           !juce::exactlyEqual(before.pitch, effectControls_.sphere.camera.pitch);
}

bool EffectEditor::clickCornerToggle(bool regionsAlways) {
    paintNow();
    const auto box = regionsAlways ? equirect.regionsToggleArea() : equirect.energyToggleArea();
    if (box.isEmpty()) return false;
    equirect.clickAt(box.getCentre());
    return true;
}

void EffectEditor::dragCornerToggle(bool regionsAlways, float dx, float dy) {
    paintNow();
    const auto box = regionsAlways ? equirect.regionsToggleArea() : equirect.energyToggleArea();
    if (box.isEmpty()) return;
    equirect.dragBetween(box.getCentre(), box.getCentre().translated(dx, dy));
}

bool EffectEditor::clickFull() {
    paintNow();
    const auto box = equirect.fullToggleArea();
    if (box.isEmpty()) return false;
    equirect.clickAt(box.getCentre());
    return true;
}

std::vector<ui::SceneRegion> EffectEditor::sceneRegions() {
    std::vector<ui::SceneRegion> into;
    effectControls_.regionsForScene(into);
    return into;
}

double EffectEditor::sceneRegionYaw(int which) {
    paintNow();
    const auto& drawn = equirect.regionsDrawnNow();
    return which >= 0 && which < static_cast<int>(drawn.size()) ? drawn[static_cast<std::size_t>(which)].region.yaw
                                                                : std::numeric_limits<double>::quiet_NaN();
}

void EffectEditor::chooseRegionKind(int slot, int kind) {
    effectControls_.applyEdit("region kind", [slot, kind](PluginState& s) {
        auto& r = s.regions[static_cast<std::size_t>(std::clamp(slot, 0, kMaxRegions - 1))].shape;
        //  the six kinds a shape can be; custom is the flag beside it, cleared by picking a shape
        r.kind = static_cast<RegionKind>(std::clamp(kind, 0, static_cast<int>(RegionKind::Clouds)));
        r.custom = false;
        r = sanitised(r);
    });
    tick();
}

}  // namespace bambi::editor
