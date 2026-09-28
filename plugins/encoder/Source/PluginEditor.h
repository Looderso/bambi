// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <limits>

#include "PluginProcessor.h"
#include "UI/Controls.h"
#include "UI/Panels.h"
#include "UI/ParameterTabs.h"
#include "UI/SceneModel.h"
#include "UI/SphereView.h"
#include "bambi/editor/PluginEditor.h"

/*  The encoder's window.
 *
 *  The window is every plugin's -- `editor::PluginEditor`: the frame, the scaling, the timer, the
 *  settings page, and the hooks a check drives it by. What is the encoder's is its scene -- the
 *  globe and the equirect with every instance's source on them, and the path of the selected one,
 *  edited in place -- the instance the controls are on, selected there -- and the host readout
 *  behind a button on its settings page.
 */
namespace bambi::ui {
/*  Built before the shared window, which is handed the controls, the scene's instance list, the tabs
    and the two views: a base is constructed before the base that follows it, and a member is not. */
struct EncoderWindowParts {
    explicit EncoderWindowParts(BambiEncoderProcessor& p);
    bambi::LinkScene scene;
    SceneState state;
    ControlState controls;
    FrameTrace trace;
    SphereView globe, equirect;
    ParameterTabs tabs;
    DiagnosticsView diagnostics;
};
}  // namespace bambi::ui

class BambiEncoderEditor final : private bambi::ui::EncoderWindowParts, public bambi::editor::PluginEditor {
public:
    explicit BambiEncoderEditor(BambiEncoderProcessor& p);

    // ---- for tools: what only the encoder's window has -----------------------------------------
    /// Open a parameter tab, a matrix tab and a provisional row, as the rendered snapshot wants them.
    void showControls(int tab, bambi::MatrixTab matrixTab, bambi::ParamId provisional = bambi::kNoParamId);
    /// Choose the region's kind, as a click on the kind list does.
    void chooseRegionKind(int kind);
    /// Select a node of the custom chain, so the snapshot shows its handles.
    void showNode(int index);

    /*  A press and release at a direction on the sphere, through the scene view's own mouse
        handlers. `onGlobe` picks the view. */
    void clickScene(bambi::Vec3 at, bool onGlobe = true);
    void dragScene(bambi::Vec3 from, float dx, float dy, bool onGlobe = true);
    /*  The same drag delivered as `steps` mouse-drag events: an orbit that accumulated from the press
        rather than from the previous move would turn the globe further the more events it got, and a
        one-step drag cannot tell the two apart. */
    void dragSceneSteps(bambi::Vec3 from, float dx, float dy, int steps, bool onGlobe = true);
    bambi::Camera sceneCamera() const { return state.camera; }
    /// Click the globe's `index`-th view preset -- top, front, side -- where the strip drew it.
    bool clickViewPreset(int index);
    bambi::ViewPreset viewPreset() const { return state.preset; }
    bambi::Uuid sceneSelected() const { return state.selected; }
    int sceneInstanceCount() const { return static_cast<int>(state.instances.size()); }
    bool sceneIsEditing() const { return state.editing; }
    int sceneSelectedNode() const { return state.selectedNode; }
    bambi::Vec3 sceneNodePosition(int index) const;
    bambi::Vec3 scenePositionOf(const bambi::Uuid& id) const {
        const auto* found = state.find(id);
        return found != nullptr && found->hasPosition ? found->position : bambi::Vec3{};
    }
    /// Select an instance in the scene, as a click on its source does.
    void selectInstance(const bambi::Uuid& instance);

    void setEquirectFull(bool full);
    bool equirectFull() const { return state.equirectFull; }
    void clickFull();
    void clickRegionsAlways();
    /*  Press `regions always` and drag off it before releasing: the toggle acts on the click, not on
        the press -- so a press that drags off it can be abandoned, as every control here allows. */
    void dragOffRegionsAlways(float dx);
    int regionsDrawnInScene() const { return equirect.regionsDrawn(); }
    int regionBuilds() const { return equirect.regionBuilds(); }
    /*  The yaw the scene drew the region at: the parameter, plus whatever the region's own rate has
        turned it by since. Paints first. NaN when the scene drew no region. */
    double sceneRegionYaw() {
        paintNow();
        const auto& drawn = equirect.regionsDrawnNow();
        return drawn.empty() ? std::numeric_limits<double>::quiet_NaN() : drawn.front().region.yaw;
    }

    /// Click a chevron of the temporary source tab. False when it was not drawn.
    bool touchSourceStep(bool forward);
    /*  Whether the tab in view drew a live mark for this parameter -- where the engine has an angle a
        rate is turning, beside the parameter it was set from -- and the value it drew. Paints first,
        so it reports what a paint pass actually produced rather than recomputing it. */
    bool liveMark(bambi::ParamId id, double& out);
    /// Whether the selected instance's path is drawn a second time, where its engine has it.
    bool livePathDrawn() {
        tick();
        const auto* shown = state.find(state.selected);
        return shown != nullptr && !shown->livePath.empty();
    }
    juce::Rectangle<float> liveMarkBounds();

private:
    int shownNode_{-1};  ///< the node the trajectory tab last showed
    void frame(bambi::ui::EditorFrame& parts) override;
    void addOwnSettings(bambi::ui::SettingsContent& c) override;
    void settingsShowing(bool) override { diagnostics.setVisible(false); }
    /// At once, as an effect's probes are: a host that throttles timers must not leave the scene's region behind a tile.
    void controlsChanged() override {
        globe.repaint();
        equirect.repaint();
    }
    int renderedOrder() const override { return processor_.diagnostics().order.load(); }

    void sceneChanged();     ///< selection, hover or camera moved
    bool updateSelection();  ///< true when the selection moved to or from another instance, or was renamed
    /// The selected instance's path as its engine built it, beside the set one, when the two differ.
    void updateLivePath();
    bambi::Trajectory livePath_;  ///< built only when the engine's settings move
    bambi::TrajectoryState liveBuiltFrom_{};

    BambiEncoderProcessor& processor_;
    bool ready_{true};  ///< whether the shown instance's controls had arrived

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiEncoderEditor)
};
