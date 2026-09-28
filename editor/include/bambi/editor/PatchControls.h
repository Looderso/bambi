// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string_view>

#include "bambi/host/ControlTarget.h"
#include "bambi/ui/MatrixModel.h"

/*  What a window shows and edits: a copy of the patch, refreshed every frame, and the editor's own
 *  view state -- which tab is open, whose settings, the provisional matrix row.
 *
 *  The instance it shows is a `host::ControlTarget`: this one, or another in the session. The patch
 *  is only ever changed through undoable edits, host parameters only inside gestures; nothing here
 *  writes to either directly.
 *
 *  A plugin derives from this and adds what is its own: which tap is selected, how an instance is
 *  chosen, what a probe answers. Members are public because the window's components read them.
 */
namespace bambi::editor {

class PatchControls : public ui::MatrixModel {
public:
    /// `ownTabs`: how many tabs the plugin's panel has of its own, before the temporary one.
    PatchControls(host::TargetHost& host, const ParamManifest& params, const ModManifest& mod, int ownTabs);

    //  ---- PatchModel -------------------------------------------------------------------------
    const ParamManifest& manifest() const final { return params_; }
    const ModManifest& modManifest() const final { return mod_; }
    const PluginState& state() const final { return patch; }

    ParamId provisionalRow() const final { return provisional; }
    void setProvisionalRow(ParamId at) final { provisional = at; }

    bool showingAnother() const final { return remote; }
    /*  What the shown instance's engine published this step, looked up in the scene the editor reads each
        frame -- by id, so nothing points into a scene that has been read again since. The editor says which
        scene, once, and which instance is shown. */
    bool engineValue(ParamId id, double& out) const override {
        const auto* entry = liveScene != nullptr ? liveScene->find(shownInstance) : nullptr;
        const int at = static_cast<int>(id);
        if (entry == nullptr || entry->dyn.sampledUs == 0 || at < 0 || at >= static_cast<int>(entry->dyn.liveCount))
            return false;
        out = static_cast<double>(entry->dyn.live[at]);
        return true;
    }
    const LinkScene* liveScene{nullptr};
    Uuid shownInstance;
    bool anotherReady() final { return target().ready(); }

    void beginParameter(ParamId at) final { target().beginParameter(at); }
    void setParameter(ParamId at, float value) final { target().setParameter(at, value); }
    void endParameter(ParamId at) final { target().endParameter(at); }

    void applyEdit(std::string_view name, const UndoStack::Edit& change) final { edit(name, change); }
    void applyEditDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) final {
        editDrag(name, key, change);
    }
    void finishDrag() final { endDrag(); }
    void notifyChanged() final { notify(); }
    /// A region's turn is integrated on the audio thread of whichever instance is shown.
    void zeroRegionTurn(int slot, int axis) final { target().zeroRegionTurn(slot, axis); }

    double sourceValueOf(int slot) final { return static_cast<double>(target().sourceValue(slot)); }
    bool canLearnSource() const final { return true; }  ///< every plugin has envelopes, and arms one for a note
    int sourceLearning() const final { return target().learning(); }
    void learnSource(int envelope) final { target().learn(envelope); }
    int panelTab() const final { return tab; }
    void showPanelTab(int index) final { showTab(index); }
    int openSourceSlot() const final { return source; }
    bool sourceTabShown() const final { return tab == kSourceTab; }
    void showSourceTab() final {
        tab = kSourceTab;
        notify();
    }
    void closeSourceTab() final { closeSource(); }
    void stepSourceTab(int delta) final { stepSource(delta); }

    int envelopeTab() const final { return envelopeTab_; }
    void setEnvelopeTab(int which) final { envelopeTab_ = which; }
    int regionTab() const final { return regionTab_; }
    void setRegionTab(int which) final { regionTab_ = which; }

    //  ---- MatrixModel ------------------------------------------------------------------------
    MatrixTab currentTab() const final { return matrixTab; }
    void setCurrentTab(MatrixTab t) final { matrixTab = t; }
    void openSourceSettings(int slot) final { openSource(slot); }

    //  ---- the instance -----------------------------------------------------------------------
    host::LocalTarget local;
    host::RemoteTarget remoteTarget;  ///< followed while another instance is selected
    bool remote{false};               ///< another instance is selected

    /// The instance every control shows and edits: this one, or the selected other.
    host::ControlTarget& target() { return remote ? static_cast<host::ControlTarget&>(remoteTarget) : local; }
    const host::ControlTarget& target() const {
        return remote ? static_cast<const host::ControlTarget&>(remoteTarget) : local;
    }

    //  ---- what is shown ----------------------------------------------------------------------
    PluginState patch;
    MatrixTab matrixTab{MatrixTab::Features};
    /*  A panel's own tabs count from zero -- the encoder has three, an effect two -- and
        `kSourceTab` is the temporary one a matrix column opens, after them. An id, not a position. */
    static constexpr int kSourceTab = 3;
    int tab{0};
    int source{-1};       ///< whose settings are open: a slot, or none
    int sourceReturn{0};  ///< the tab they replaced
    int envelopeTab_{0};  ///< shape or trigger
    int regionTab_{0};    ///< shape or transform
    ParamId provisional{kNoParamId};

    std::function<void()> changed;  ///< repaint the matrix and the panel

    /// Re-read the patch. True when anything the window draws has changed.
    bool refresh();
    void notify() const;
    float value(ParamId id) const { return patch.params[static_cast<std::size_t>(id)]; }
    bool isRow(ParamId id) const;  ///< it already has a row, so a provisional one would be a copy

    /// One undoable edit, seen at once.
    void edit(std::string_view name, const UndoStack::Edit& change);
    /// One step of a drag; every call with the same key is the same undo step, until endDrag.
    void editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change);
    void endDrag() { target().endDrag(); }

    /// Stop learning a note, if the shown instance is.
    void stopLearning();
    /// Open a source's settings in the temporary tab; the same source again closes them.
    void openSource(int slot);
    /// Close a source's settings, back to the tab they replaced.
    void closeSource();
    /*  The next source that has a page, or the one before, wrapping; the matrix follows, so the
        column whose settings are open is the column in view. The region is one only where it is a
        source. */
    void stepSource(int delta);
    /// Go to one of the plugin's own tabs. A source's settings, being temporary, close.
    void showTab(int index);

private:
    const ParamManifest& params_;
    const ModManifest& mod_;
    int ownTabs_;
};

}  // namespace bambi::editor
