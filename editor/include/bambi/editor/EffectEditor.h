// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <limits>
#include <vector>

#include "bambi/editor/EffectControls.h"
#include "bambi/editor/PluginEditor.h"
#include "bambi/host/FieldEffectProcessor.h"
#include "bambi/ui/ProbeView.h"

/*  A field effect's window: every plugin's window (`PluginEditor`) with two probe views where the
 *  encoder has its scene -- point at a direction and the plugin answers what it would do to a
 *  source there -- and the frame an effect runs. A plugin derives from this, hands over its
 *  control state and its panel, both built before it, and adds the hooks that are about its own
 *  panel.
 */
namespace bambi::editor {

/*  Built before the shared window, which is handed both views: a base is constructed before the
    base that follows it, and a member is not. */
struct EffectViews {
    explicit EffectViews(EffectControls& controls);
    ui::ProbeView globe, equirect;
};

class EffectEditor : private EffectViews, public PluginEditor {
public:
    //  ---- the header's instance list --------------------------------------------------------------
    int instanceCount() const { return effectControls_.instanceCount(); }
    juce::String instanceLabel(int index) const { return effectControls_.instanceLabel(index); }
    void selectInstance(const Uuid& id) { effectControls_.selectInstance(id); }
    void selectSelf() { effectControls_.selectSelfInstance(); }
    bool showingAnother() const { return effectControls_.showingAnother(); }
    bool anotherReady() { return effectControls_.anotherReady(); }

    //  ---- the two views: the probe, the camera, the corner toggles, the regions ----------------
    /// Set the probe where a normalised position on a view falls. False when that is off the sphere.
    bool placeProbe(double nx, double ny, bool onGlobe = true);
    /// The same position through the view's own mouse handlers, which is what checks that a click places it and a drag never does.
    void clickProbe(double nx, double ny, bool onGlobe = true);
    void dragProbe(double fromNx, double fromNy, double toNx, double toNy, bool onGlobe = true);
    Vec3 probeDirection() const { return effectControls_.sphere.at; }
    bool probePlaced() const { return effectControls_.sphere.placed; }
    void answerProbe(Vec3 from, ui::ProbeReply& into) { effectControls_.answerProbe(from, into); }

    Camera camera() const { return effectControls_.sphere.camera; }
    /// Click the globe's `index`-th view preset -- top, front, side -- where the strip drew it.
    bool clickViewPreset(int index);
    ViewPreset viewPreset() const { return effectControls_.sphere.preset; }
    bool cameraMoved(Camera before) const;
    /// The scene corner. `regionsAlways` picks which toggle. False when it was not drawn.
    bool clickCornerToggle(bool regionsAlways);
    /// Press a corner toggle and drag from it, to prove the press starts no orbit.
    void dragCornerToggle(bool regionsAlways, float dx, float dy);
    bool clickFull();
    void setEquirectFull(bool full) {
        effectControls_.sphere.equirectFull = full;
        relayout();
    }
    bool equirectFull() const { return effectControls_.sphere.equirectFull; }
    bool globeShown() const { return globe.isVisible(); }
    int equirectWidth() const { return equirect.getWidth(); }
    int equirectWidthNow() const { return equirect.getWidth(); }
    void clickEquirectAt(juce::Point<float> at) { equirect.clickAt(at); }
    int regionsDrawn() const { return equirect.regionsDrawn(); }
    bool energyDrawn() const { return equirect.energyDrawn(); }
    /// The patch as the shown instance plays it, from what its engine published.
    PluginState patchAsPlayed() const { return ui::patchAsPlayed(effectControls_); }

    /*  The regions exactly as the scene draws them, turn included: a check that re-derives the
        region from the parameters is checking its own arithmetic and not the picture. */
    std::vector<ui::SceneRegion> sceneRegions();
    /// The yaw the scene drew a region at, which is the parameter plus what the rates turned it by. NaN when it drew fewer.
    double sceneRegionYaw(int which);
    void chooseRegionKind(int slot, int kind);
    int regionRepaintsAsked() const { return globe.regionRepaintsAsked() + equirect.regionRepaintsAsked(); }

protected:
    /// `controls` and `panel` must already be built. `levels` and `footerSwitches` are the plugin's, by its own parameters.
    EffectEditor(host::FieldEffectProcessor& processor, EffectControls& controls, ui::ParameterPage& panel,
                 const juce::String& plugin, std::vector<ui::LevelColumn::Level> levels,
                 std::vector<ui::StatusFooter::Switch> footerSwitches);

    /// What this effect's settings page has beside the suite's and the host readout: nothing, unless it says so.
    virtual void addEffectSettings(ui::SettingsContent&) {}

private:
    void frame(ui::EditorFrame& frame) final;
    void controlsChanged() final;
    void addOwnSettings(ui::SettingsContent& c) final;
    int renderedOrder() const final { return effect_.engineOrder(); }

    host::FieldEffectProcessor& effect_;
    EffectControls& effectControls_;
};

}  // namespace bambi::editor
