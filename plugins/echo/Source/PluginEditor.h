// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "UI/Controls.h"
#include "UI/Panel.h"
#include "bambi/editor/EffectEditor.h"

/*  Echo's window.
 *
 *  The window is every field effect's -- `editor::EffectEditor`. What is Echo's is its control
 *  state, its panel -- the tap selector with the strip pinned over it -- and the hooks below that
 *  are about taps.
 */
namespace bambi::ui {
/*  Built before the shared window, which is handed both: a base is constructed before the base
    that follows it, and a member is not. */
struct EchoWindowParts {
    explicit EchoWindowParts(BambiEchoProcessor& p) : controls(p), panel(controls) {}
    EchoControlState controls;
    EchoPanel panel;
};
}  // namespace bambi::ui

class BambiEchoEditor final : private bambi::ui::EchoWindowParts, public bambi::editor::EffectEditor {
public:
    explicit BambiEchoEditor(BambiEchoProcessor& p);

    //  ---- for tools: what a click on the tap selector would do --------------------------------
    /// Select a tap, as a click on its row does.
    void selectTap(int index);
    /// Open a category of the selected tap.
    void showCategory(int index);
    /// Click a tap's row where the row itself is, not its switch or its level.
    bool clickTapRow(int index);
    /// Click a tap's on/off switch, as the mouse does.
    bool clickTapSwitch(int index);
    /// Whether the panel drew this instance's controls, rather than the remote notice.
    bool panelDrawsControls() const { return !panel.drawnArea(controls.tapParam(0, "level")).isEmpty(); }

    int selectedTap() const { return controls.tap; }
    int shownCategory() const { return controls.category; }
    const bambi::ui::EchoControlState& controlState() const { return controls; }

private:
    /// The selector's switches are not tiles: they carry no value box.
    bool drawnOnPanel(bambi::ParamId id) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiEchoEditor)
};
