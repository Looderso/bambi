// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/editor/PatchControls.h"
#include "bambi/ui/InstanceModel.h"
#include "bambi/ui/ProbeView.h"
#include "bambi/ui/RegionEditor.h"

/*  A field effect's window state: `PatchControls`, the list of instances its header's tabs select
 *  from, and the sphere. An effect has no scene of sources to pick another instance in, so its list
 *  is the header's own kind only.
 */
namespace bambi::editor {

class EffectControls : public PatchControls, public ui::InstanceModel {
public:
    /// This plugin's region slots, in the order the scene draws them, and the panel tab each is edited on: slot k on `firstTab + k`.
    struct Regions {
        int firstTab{1};
        std::vector<ui::RegionSlot> slots;
    };
    EffectControls(host::TargetHost& host, const ParamManifest& params, const ModManifest& mod, int ownTabs,
                   Regions regions);

    //  ---- InstanceModel ----------------------------------------------------------------------
    int instanceCount() const final { return static_cast<int>(instances.size()); }
    Uuid instanceAt(int index) const final;
    juce::String instanceLabel(int index) const final;
    Uuid selectedInstance() const final { return selected; }
    /// Selecting an instance moves every control onto it; whatever was still open on the one being left is ended first.
    void selectInstance(const Uuid& id) final;

    juce::String anotherLabel() const final;
    void selectSelfInstance() final;

    struct Instance {
        Uuid id;
        juce::String label;
    };
    std::vector<Instance> instances;
    Uuid selected;  ///< which one the window is showing; this one by default
    LinkScene scene;

    /// Re-read what the bus is carrying. True when the list or the selection changed.
    bool rebuildInstances();

    /// The sphere: where the probe is pointed, and how the scene is looked at. Editor state, not the patch's.
    ui::ProbeView::State sphere;

    //  ---- what differs between effects, and the shared window asks ----------------------------
    /// What the plugin would do to a source in that direction: Echo's spirals, Reverb's early reflections.
    virtual void answerProbe(Vec3 from, ui::ProbeReply& into) = 0;
    /*  The slot whose tab is open: the one that takes handles. Slot 0 when no region tab is open --
        a source's page asks for one to copy a region into or out of. */
    int editedRegion() const;
    /// Whether `which` is the slot its open tab shows.
    bool regionOpen(int which) const { return tab == regions_.firstTab + which; }

    /// Its regions in the scene, turn included: the edited slot with handles and its wash, the others as an edge and a role label.
    void regionsForScene(std::vector<ui::SceneRegion>& into);
    void carryRegion(ui::RegionHandle::Kind kind, Vec3 to);
    void resetRegion(ui::RegionHandle::Kind kind);

protected:
    Uuid self() const;

private:
    /// Keep the remote target on the selected peer; come back here when it leaves.
    void followSelected();

    void turnOf(int slot, double& yaw, double& pitch, double& roll) const;

    host::TargetHost& host_;
    Regions regions_;
    const ui::RegionSlot* edited() const;  ///< the slot the regions tab shows, or null when there is none
};

}  // namespace bambi::editor
