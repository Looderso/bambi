// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/editor/EffectControls.h"

#include <algorithm>

#include "bambi/ui/EditorFrame.h"

namespace bambi::editor {

EffectControls::EffectControls(host::TargetHost& host, const ParamManifest& params, const ModManifest& mod, int ownTabs,
                               Regions regions)
    : PatchControls(host, params, mod, ownTabs), host_(host), regions_(std::move(regions)) {}

Uuid EffectControls::self() const { return host_.linkHost().identity().instance; }

Uuid EffectControls::instanceAt(int index) const {
    return index >= 0 && index < instanceCount() ? instances[static_cast<std::size_t>(index)].id : Uuid{};
}

juce::String EffectControls::instanceLabel(int index) const {
    return index >= 0 && index < instanceCount() ? instances[static_cast<std::size_t>(index)].label : juce::String();
}

bool EffectControls::rebuildInstances() {
    host_.linkNode().updateScene(scene);
    followSelected();  // the remote target rides the scene it was selected from
    const auto before = instances.size();
    const auto wasSelected = selected;

    const auto product = host_.linkHost().product();
    instances.clear();
    for (const auto& entry : scene.entries()) {
        if (entry.product != product) continue;
        instances.push_back({entry.instance, juce::String(entry.label())});
    }

    //  the selection must name something that is there; falling back to this instance is the truth
    const bool present =
        std::any_of(instances.begin(), instances.end(), [this](const Instance& i) { return i.id == selected; });
    if (!present) selected = self();
    liveScene = &scene;  // what its engine plays, for the pictures and the live marks
    shownInstance = selected;
    return instances.size() != before || !(selected == wasSelected);
}

juce::String EffectControls::anotherLabel() const {
    for (const auto& instance : instances)
        if (instance.id == selected) return instance.label;
    return {};
}

void EffectControls::selectSelfInstance() { selectInstance(self()); }

void EffectControls::selectInstance(const Uuid& id) {
    if (selected == id) return;
    remoteTarget.release();
    selected = id;
    remote = !(id == self());
    if (remote) followSelected();
    notify();
}

void EffectControls::followSelected() {
    if (!remote) return;
    for (const auto& entry : scene.entries())
        if (entry.instance == selected) {
            remoteTarget.follow(entry, 1.0 / ui::kFrameHz);
            return;
        }
    //  it left the session: come back to this instance rather than show a patch nobody is behind
    selectSelfInstance();
}

void EffectControls::turnOf(int slot, double& yaw, double& pitch, double& roll) const {
    yaw = static_cast<double>(host_.regionTurn(slot, 0));
    pitch = static_cast<double>(host_.regionTurn(slot, 1));
    roll = static_cast<double>(host_.regionTurn(slot, 2));
}

void EffectControls::regionsForScene(std::vector<ui::SceneRegion>& into) {
    for (int which = 0; which < static_cast<int>(regions_.slots.size()); ++which) {
        const auto& slot = regions_.slots[static_cast<std::size_t>(which)];
        double yaw = 0.0, pitch = 0.0, roll = 0.0;
        if (!remote)  // another instance's turn is not on the bus: one selected from here is drawn as set
            turnOf(slot.index, yaw, pitch, roll);
        const auto region = ui::regionOf(*this, slot, yaw, pitch, roll);
        if (region.kind == RegionKind::Everywhere)
            continue;  // everywhere is the whole sphere: an edge round all of it says nothing
        into.push_back({region, slot.role, regionOpen(which), true});
    }
    //  what the engine plays, beside each, after all of them so a slot's own entry keeps its place
    for (const auto& slot : regions_.slots) {
        double yaw = 0.0, pitch = 0.0, roll = 0.0;
        if (!remote) turnOf(slot.index, yaw, pitch, roll);
        ui::appendLiveRegion(scene, selected, slot.index, ui::regionOf(*this, slot, yaw, pitch, roll), into);
    }
    ui::appendOtherRegions(scene, host_.linkHost().product(), selected, into);  // the others of this kind
}

int EffectControls::editedRegion() const {
    const int which = tab - regions_.firstTab;
    return which >= 0 && which < static_cast<int>(regions_.slots.size()) ? which : 0;
}

const ui::RegionSlot* EffectControls::edited() const {
    const int which = editedRegion();
    return which >= 0 && which < static_cast<int>(regions_.slots.size())
               ? &regions_.slots[static_cast<std::size_t>(which)]
               : nullptr;  // a plugin with no regions: nothing to carry
}

void EffectControls::carryRegion(ui::RegionHandle::Kind kind, Vec3 to) {
    const auto* slot = edited();
    if (slot == nullptr) return;
    double yaw = 0.0, pitch = 0.0, roll = 0.0;
    turnOf(slot->index, yaw, pitch, roll);
    ui::carryRegionHandle(*this, *slot, ui::regionOf(*this, *slot, yaw, pitch, roll), kind, to, yaw, pitch, roll);
}

void EffectControls::resetRegion(ui::RegionHandle::Kind kind) {
    const auto* slot = edited();
    if (slot == nullptr) return;
    double yaw = 0.0, pitch = 0.0, roll = 0.0;
    turnOf(slot->index, yaw, pitch, roll);
    ui::resetRegionHandle(*this, *slot, ui::regionOf(*this, *slot, yaw, pitch, roll), kind);
}

}  // namespace bambi::editor
