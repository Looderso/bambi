// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "UI/Controls.h"
#include "UI/HeadStrip.h"
#include "UI/Panel.h"
#include "UI/SceneView.h"
#include "bambi/editor/PluginEditor.h"

/*  The decoder's window: every plugin's window (`editor::PluginEditor`) with a different centre. One scene in
 *  the room's frame -- sources, loudspeakers, a head -- where the effects have their probes; the head strip where
 *  they have their matrix; the outputs as the header's tabs, where they keep their instances, the name opening an
 *  output's page and a square in the tab playing it; the open output's page in the right panel. The sketch is
 *  prototypes/ui/decoder/.
 */
namespace bambi::ui {
/*  Built BEFORE the shared window, which is handed the parts: a base is constructed before the base that
    follows it, and a member is not. */
struct DecoderWindowParts {
    explicit DecoderWindowParts(BambiDecoderProcessor& p)
        : controls(p),
          panel(controls),
          strip(controls),
          globe(controls, Projection::Globe),
          equirect(controls, Projection::Equirect) {}
    DecoderControlState controls;
    DecoderPanel panel;
    HeadStrip strip;
    DecoderSceneView globe, equirect;
};
}  // namespace bambi::ui

class BambiDecoderEditor final : private bambi::ui::DecoderWindowParts, public bambi::editor::PluginEditor {
public:
    explicit BambiDecoderEditor(BambiDecoderProcessor& p);
    ~BambiDecoderEditor() override;

    //  ---- for tools ------------------------------------------------------------------------
    /// Open an output's page: 0 headphones, 1 speakers, 2 lens.
    void showOutput(int index);
    /// Click the square in an output's tab, as the mouse would.
    bool clickOutputSquare(int index);
    int shownOutput() const { return static_cast<int>(controls.page); }
    bool hullDrawn() const { return globe.hullDrawn(); }
    int speakersDrawnInScene() const { return equirect.speakersDrawn(); }
    bambi::ui::DecoderPanel& decoderPanel() { return panel; }
    bambi::ui::HeadStrip& headStrip() { return strip; }

private:
    void frame(bambi::ui::EditorFrame& parts) override;
    void controlsChanged() override;
    void addOwnSettings(bambi::ui::SettingsContent& c) override;
    bool drawnOnPanel(bambi::ParamId id) override { return !strip.drawnArea(id).isEmpty(); }
    int renderedOrder() const override { return controls.renderedOrder(); }

    BambiDecoderProcessor& processor_;
    int trackerMessagesSeen_{0};
    double trackerRateAt_{0.0};
    float trackerRate_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiDecoderEditor)
};
