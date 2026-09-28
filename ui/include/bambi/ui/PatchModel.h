// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <juce_core/juce_core.h>
#include <string_view>

#include "bambi/mod/manifest.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/patch/undo.hpp"

namespace bambi::ui {

/*  The plugin's patch, as shared components read and change it. The parameter pages want the same
 *  things as the matrix -- the manifest, the patch, a gesture on a host parameter, an undoable edit --
 *  but only those; `MatrixModel` derives from this and adds what the matrix needs beyond it.
 *
 *  Parameters are `ParamId`, an opaque handle declared in `patch/` and defined by each plugin: `ui/`
 *  can hold one, pass one and cast one, and simply cannot name one.
 *
 *  Everything below is called while painting or handling a mouse, never on the audio thread.
 */
class PatchModel {
public:
    virtual ~PatchModel() = default;

    //  ---- what is being shown ------------------------------------------------------------------
    virtual const ParamManifest& manifest() const = 0;
    virtual const ModManifest& modManifest() const = 0;
    virtual const PluginState& state() const = 0;

    /// The assign flow's provisional row: a target named but not yet given depth. kNoParam for none.
    virtual ParamId provisionalRow() const = 0;
    virtual void setProvisionalRow(ParamId at) = 0;

    //  ---- another instance ------------------------------------------------------------------------
    /// The order the shown instance renders at. The full range when a plugin cannot say.
    virtual int renderedOrder() const { return kRegionWeightsOrder; }
    /// A window may be showing an instance that is not its own. Until that one has answered there is
    /// nothing to draw, and the notice offers the way back. False by default: a plugin showing only
    /// itself never shows another instance, so the notice never appears.
    virtual bool showingAnother() const { return false; }
    /// What the engine of the instance shown has this parameter at, as published: the tiles' live
    /// mark, and a picture of what plays. False when nothing has been published.
    virtual bool engineValue(ParamId, double&) const { return false; }
    virtual bool anotherReady() { return true; }
    virtual juce::String anotherLabel() const { return {}; }
    virtual void selectSelfInstance() {}

    //  ---- editing ------------------------------------------------------------------------------
    /// An amount is a host parameter and moves inside a gesture; a depth is state and moves as an
    /// undoable edit. Nothing here writes to either directly.
    virtual void beginParameter(ParamId at) = 0;
    virtual void setParameter(ParamId at, float value) = 0;
    virtual void endParameter(ParamId at) = 0;

    virtual void applyEdit(std::string_view name, const UndoStack::Edit& change) = 0;
    virtual void applyEditDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) = 0;
    virtual void finishDrag() = 0;

    /// Something the matrix changed: repaint whatever else shows it.
    virtual void notifyChanged() = 0;

    /// The panel's tabs, and the temporary one a source's settings open in. The page draws the bar;
    /// which tab is open is the window's state. A model with no tabs answers with the defaults.
    virtual int panelTab() const { return 0; }
    virtual void showPanelTab(int) {}
    virtual int openSourceSlot() const { return -1; }  ///< whose settings the temporary tab holds, or none
    virtual bool sourceTabShown() const { return false; }
    virtual void showSourceTab() {}
    virtual void closeSourceTab() {}
    virtual void stepSourceTab(int) {}

    //  ---- what a source's own settings page asks ---------------------------------------------------
    virtual double sourceValueOf(int slot) = 0;  ///< not const: it may have to ask another instance
    virtual bool canLearnSource() const { return false; }
    /// The source the envelope is waiting for a note from, to set its trigger, or -1. Answered by
    /// doing nothing, so a plugin without note learn draws the button inert rather than pretending.
    virtual int sourceLearning() const { return -1; }
    virtual void learnSource(int) {}
    virtual int envelopeTab() const { return 0; }
    virtual void setEnvelopeTab(int) {}
    virtual int regionTab() const { return 0; }
    virtual void setRegionTab(int) {}

    /// Clear a region's accumulated turn on one axis -- 0 yaw, 1 pitch, 2 roll -- because its angle
    /// was set back to zero by hand. The turn is integrated on the audio thread, so only the plugin
    /// can reach it; what is shared is knowing when. Answered by doing nothing by default.
    virtual void zeroRegionTurn(int, int) {}

    //  ---- conveniences the view would otherwise spell out every time ----------------------------
    float valueOf(ParamId at) const { return at == kNoParamId ? 0.0f : state().params[static_cast<std::size_t>(at)]; }
    const ParamDesc& desc(ParamId at) const { return manifest()[static_cast<int>(at)]; }

    /*  Set a parameter outside a drag -- a switch stepping, a double-click to the default. A gesture
        of one, so a host records it as an edit rather than as a value appearing from nowhere. */
    void changeParameter(ParamId at, float value) {
        beginParameter(at);
        setParameter(at, value);
        endParameter(at);
    }

    /// Something drives it: what the underline under a tile's name means.
    bool committed(ParamId at) const { return targetHasDepth(state(), at); }

    /// It already has a matrix row, so a provisional one would be a copy.
    bool hasRow(ParamId at) const {
        const auto rows = matrixTargets(modManifest(), state());
        return std::find(rows.begin(), rows.end(), at) != rows.end();
    }
};

/*  The patch as it plays: the shown state with every parameter the engine has published put in place
    of what is set. What a picture of what is heard is drawn from -- Echo's strip and ping. Where
    nothing has been published, it is the patch as set. */
inline PluginState patchAsPlayed(const PatchModel& model) {
    PluginState played = model.state();
    for (int i = 0; i < model.manifest().size(); ++i) {
        double v = 0.0;
        if (model.engineValue(static_cast<ParamId>(i), v))
            played.params[static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
    return played;
}

}  // namespace bambi::ui
