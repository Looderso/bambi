// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "UI/Controls.h"
#include "UI/HeadStrip.h"
#include "UI/Panel.h"
#include "UI/SceneView.h"
#include "bambi/editor/PluginEditor.h"

/*  The decoder's window: every plugin's window (`editor::PluginEditor`) with a different centre. One scene in
 *  the room's frame -- sources, loudspeakers, a head -- where the effects have their probes; the orientation strip
 *  where they have their matrix; the outputs as the header's tabs, where they keep their instances, the name
 *  opening an output's page and a square in the tab playing it; the open output's page in the right panel.
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
    /// Open an output's page: 0 headphones, 1 speakers.
    void showOutput(int index);
    /// Open one of the panel's temporary tabs, as its button would.
    void openTemp(bambi::ui::TempTab tab) { controls.openTemp(tab); }
    /// Switch the energy picture on, as the equirect's toggle would.
    void showEnergy() { controls.sphere.energyOn = true; }
    //  the session's decoders, as the header's switch lists them after a frame
    int sessionDecoders() const { return static_cast<int>(controls.decoders.size()); }
    void showDecoder(int index) {
        if (index >= 0 && index < sessionDecoders())
            controls.shownDecoder = controls.decoders[static_cast<std::size_t>(index)].first;
        tick();
    }
    bool showingAnother() const { return controls.remote; }
    juce::String shownLayoutName() const { return controls.decoder().layoutName; }
    juce::Rectangle<float> instanceSwitchArea(int index) { return header().instanceSwitchArea(index); }
    /// Click the square in an output's tab, as the mouse would.
    bool clickOutputSquare(int index);

private:
    void frame(bambi::ui::EditorFrame& parts) override;
    //  the globe's sources toggle and its list, through the painted areas as the mouse would: each paints first, and
    //  is false when the control is not there to press
    bool clickSourcesToggle();
    bool clickSourcesTriangle();
    bool clickSourcesRow(int index);
    bool clickSourcesAll(bool on);
    bool clickGlobeAt(juce::Point<float> at);  ///< anywhere on the globe's view, as a press and release
    int sourcesListed() const { return static_cast<int>(controls.sources.rows().size()); }
    int sourcesDrawn() const { return globe.sourcesDrawn(); }
    bool sourcesMenuOpen() const { return controls.sources.open(); }
    /// How many choices the list holds: an instance that left must not leave one behind.
    int sourcesChoices() const { return static_cast<int>(controls.sources.selection().choices()); }
    bool sourcesMasterOn() const { return controls.sources.selection().master(); }
    /// What the list's rows are, in order, as the list was last drawn: a row scrolled out of sight is empty.
    juce::Rectangle<float> sourcesRowArea(int index) const;
    juce::Rectangle<float> sourcesMenuBox() const { return controls.sources.listAreas().box; }
    const bambi::ui::OverlayMenuAreas& sourcesMenuAreas() const { return controls.sources.listAreas(); }
    /// A key pressed with the globe focused, as the keyboard would: true when the list took it.
    bool pressKeyOnGlobe(const juce::KeyPress& key) { return globe.keyPressed(key); }
    bool dragGlobeBetween(juce::Point<float> from, juce::Point<float> to);  ///< a drag on the globe, as the mouse's
    double globeCameraYaw() const { return controls.sphere.camera.yaw; }

    void controlsChanged() override;
    void addOwnSettings(bambi::ui::SettingsContent& c) override;
    bool drawnOnPanel(bambi::ParamId id) override { return !strip.drawnArea(id).isEmpty(); }
    int renderedOrder() const override { return controls.renderedOrder(); }

    BambiDecoderProcessor& processor_;
    int listFrames_{0};  ///< frames since the window opened, for learn mode's list
    int trackerMessagesSeen_{0};
    double trackerRateAt_{0.0};
    float trackerRate_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiDecoderEditor)
};
