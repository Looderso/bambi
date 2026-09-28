// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "UI/Controls.h"
#include "UI/Panel.h"
#include "bambi/editor/EffectEditor.h"

/*  Reverb's window.
 *
 *  The window is every field effect's -- `editor::EffectEditor`. What is Reverb's is its control
 *  state, its panel, the two things its settings page and footer carry that Echo's do not -- the
 *  quality while playing and when rendering -- and the hooks below that are about the room's
 *  presets and the two region slots.
 */
namespace bambi::ui {
/*  Built BEFORE the shared window, which is handed both: a base is constructed before the base
    that follows it, and a member is not. */
struct ReverbWindowParts {
    explicit ReverbWindowParts(BambiReverbProcessor& p) : controls(p), panel(controls) {}
    ReverbControlState controls;
    ReverbPanel panel;
};
}  // namespace bambi::ui

class BambiReverbEditor final : private bambi::ui::ReverbWindowParts, public bambi::editor::EffectEditor {
public:
    explicit BambiReverbEditor(BambiReverbProcessor& p);

    //  ---- for tools ------------------------------------------------------------------------
    /// Show the send's or the return's region, as the segment on the regions tab does.
    void showSlot(int which);
    /// Click one of the six preset buttons where it is drawn. False when it was not.
    bool clickPreset(int index);
    int shownSlot() const { return controls.editedRegion(); }

private:
    void addEffectSettings(bambi::ui::SettingsContent& c) override;

    BambiReverbProcessor& processor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiReverbEditor)
};
