// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

#include "bambi/editor/PatchControls.h"
#include "bambi/editor/ProcessorPresets.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/ui/ConfirmBox.h"
#include "bambi/ui/EditorFrame.h"
#include "bambi/ui/Header.h"
#include "bambi/ui/InstanceModel.h"
#include "bambi/ui/LevelColumn.h"
#include "bambi/ui/MatrixView.h"
#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/PresetBrowser.h"
#include "bambi/ui/SceneStrip.h"
#include "bambi/ui/SettingsPage.h"
#include "bambi/ui/StatusFooter.h"
#include "bambi/ui/Tooltips.h"

/*  Every bambi plugin's window: the header, a row of two scene views, the matrix under them, the
 *  plugin's panel beside them between two rules, the footer under the panel, the level column down
 *  the edge, and the settings page over all of it but the column and the footer. What is here is
 *  that frame, the timer, and the hooks a check drives a window by, which paint first so the regions
 *  a press lands on are already built.
 *
 *  What a plugin brings is built before this, in a private base of its own editor: its control
 *  state, the model its header's tabs list, its panel, and its two scene views. The encoder's views
 *  are a scene of sources; an effect's are two probes (`EffectEditor`).
 *
 *  This class's constructor calls nothing a derived class can answer, since it sets its size, which
 *  lays it out before a derived class exists. The virtuals below are reached only from the timer, a
 *  click or the settings page, all of which happen after construction.
 */
namespace bambi::editor {

class PluginEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    ~PluginEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Drive one frame, as the timer does.
    void tick();
    void paintNow();
    /// How the scene is looked at now. For checks.
    const ui::SceneViewState& sceneView() const { return view_; }
    /// The equirect, as the shared region checks drive it: through the mouse, and asked what it drew.
    ui::HitArea* equirectArea() { return dynamic_cast<ui::HitArea*>(&equirect_); }
    ui::RegionView* equirectRegions() { return dynamic_cast<ui::RegionView*>(&equirect_); }

    //  ---- a parameter's control, wherever it is: a tile, a level, a footer switch --------------
    /// Touch a parameter's value box, scrolled to first. False when it is not on what is in view.
    bool touchParameter(ParamId id);
    juce::Rectangle<float> tileAreaOf(ParamId id);
    void doubleClickTile(ParamId id);
    void dragTile(ParamId id, float dy);
    /// Whether this parameter has a box on what is in view. Paints first, so it reports what a paint actually produced.
    bool drawnSomewhere(ParamId id);
    /// The same, of the page alone: for a check that a control which lives in the column or the footer is not drawn twice.
    bool drawnOnPage(ParamId id);
    float shownValue(ParamId id) const { return controls_.state().params[static_cast<std::size_t>(id)]; }
    /// The patch the window is showing, as it has it now -- not as the next frame will.
    const PluginState& shownState() const { return controls_.state(); }
    ParamId provisionalRow() const { return controls_.provisional; }
    bool dragLevel(ParamId id, float dy);
    bool clickLevel(ParamId id);
    bool clickFooterSwitch(ParamId id);

    //  ---- the panel's tabs and a source's page ------------------------------------------------
    void showTab(int index);
    int shownTab() const { return controls_.tab; }
    void openSource(int slot) {
        controls_.openSource(slot);
        page_.repaint();
    }
    void showSource(int slot) { openSource(slot); }
    void stepSource(int delta) {
        controls_.stepSource(delta);
        page_.repaint();
    }
    int shownSource() const { return controls_.source; }
    void setEnvelopeTab(int which) {
        controls_.setEnvelopeTab(which);
        page_.repaint();
    }
    void setRegionTab(int which) {
        controls_.setRegionTab(which);
        page_.repaint();
    }
    /// Click a source column's head. Where it is recorded by the paint, not re-derived; clicks even where the tab has no such column.
    void clickSourceColumn(MatrixTab tab, int column);
    bool clickMatrixTab(MatrixTab tab);
    std::vector<MatrixTab> matrixTabsOffered() const { return matrix_.tabsOffered(); }
    int matrixColumnsDrawn() const { return matrix_.columnsDrawn(); }
    void selectMatrixTab(MatrixTab t) { controls_.setCurrentTab(t); }
    MatrixTab shownMatrixTab() const { return controls_.currentTab(); }

    //  ---- a source's graphs, dragged the way the mouse does ------------------------------------
    bool dragEnvelope(EnvelopeGrab grab, float dx, float dy);
    void panelDragTo(juce::Point<float> from, juce::Point<float> to) { page_.dragBetween(from, to); }
    void panelPressAt(juce::Point<float> at) { page_.pressAt(at); }
    void panelMoveTo(juce::Point<float> to) {
        page_.moveTo(to);
        tick();
        paintNow();
    }
    void panelRelease() {
        page_.releaseDrag();
        tick();
        paintNow();
    }
    ui::ParameterPage::WavePlot wavePlot() const { return page_.wavePlot(); }
    juce::Rectangle<float> envelopePlot() const { return page_.envelopeGraph().plotArea(); }
    juce::Point<float> envelopeHandle(EnvelopeGrab grab) const { return page_.envelopeGraph().handleAt(grab); }

    //  ---- something the page recorded by name: a button, not a parameter -----------------------
    bool clickNamed(const juce::String& name);
    bool namedDrawn(const juce::String& name) {
        paintNow();
        return page_.namedAreaDrawn(name);
    }
    void revealNamed(const juce::String& name);
    int panelRepaintsForClipboard() const { return page_.clipboardRepaintsAsked(); }

    //  ---- what repaints on its own, counted; and what scrolls ----------------------------------
    int liveRepaintsAsked() const { return page_.liveRepaintsAsked(); }
    int matrixHeaderRepaintsAsked() const { return matrix_.headerRepaintsAsked(); }
    int headerRepaintsAsked() const { return header_.repaintsAsked(); }
    float matrixScrollRange() const { return matrix_.scrollRange(); }
    float matrixScrollPosition() const { return matrix_.scrollPosition(); }
    void scrollMatrixTo(float y) { matrix_.scrollTo(y); }
    float tabsScrollRange() const { return page_.scrollRange(); }
    float tabsScrollPosition() const { return page_.scrollPosition(); }
    void scrollTabsTo(float y) { page_.scrollTo(y); }

    //  ---- the footer and the settings page ------------------------------------------------------
    juce::String footerReadout() const { return footer_.readout(); }
    ui::StatusFooter& footer() { return footer_; }  ///< for checks
    void openSettings(bool open) { showSettings(open); }
    bool settingsOpen() const { return settings_.isVisible(); }
    int settingsRefreshes() const { return settings_.refreshesAsked(); }

    //  ---- presets: the header's cluster, the browser over the panel, the question ---------------
    void openPresets(bool open);
    bool presetsOpen() const { return browser_.isVisible(); }
    ui::PresetBrowser& presetBrowser() noexcept { return browser_; }
    juce::String presetShown() const { return header_.presetShown(); }
    /// Click the header's preset name, or one of its chevrons, where it is drawn. False when it is greyed.
    bool clickPresetName();
    /// Click the header's icon and name, as the mouse does. False when it was not drawn.
    bool clickHome();
    bool clickPresetStep(int delta);
    /// Click a preset's row, a heading or an action in the browser, scrolled to first. False when it is not drawn or not live.
    bool clickPresetRow(const PresetRef& ref);
    bool clickPresetHeading(const std::string& heading);
    bool clickPresetAction(ui::PresetBrowser::Action action);
    bool questionShown() const { return confirm_.asking(); }
    juce::String questionAsked() const { return confirm_.question(); }
    /// Answer the question by its button, where it is drawn; or by a key, as the keyboard would.
    bool clickAnswer(bool yes);
    bool pressKeyOnQuestion(const juce::KeyPress& key) { return confirm_.asking() && confirm_.keyPressed(key); }

    //  ---- the energy picture ---------------------------------------------------------------------
    /// Switch it as the strip's toggle does. It is off when a window opens.
    void setEnergyShown(bool on) {
        view_.energyOn = on;
        tick();
    }
    bool energyShown() const { return view_.energyOn; }
    /// Whether the switch is live: it is greyed while the window shows another instance.
    bool energyAvailable() const { return view_.energyAvailable; }
    void typeInstanceName(const juce::String& text) { settings_.typeName(text); }
    juce::String instanceNameShown() const { return settings_.nameShown(); }
    bool clickSettingsOption(int choice, int option);
    /// Click the button of the page's `index`-th action -- the encoder's host readout -- where it is drawn.
    bool clickSettingsAction(int index);
    /// Click the header's undo or redo, as the mouse does. False when it is not drawn or not live.
    bool clickUndo(bool redo = false);
    /// A page of the plugin's own is covering the window, and the footer is in front of it.
    bool overlayShown() const { return overlay_ != nullptr && overlay_->isVisible(); }
    bool footerIsInFrontOfOverlay();
    bool closeSettingsByItsCross();
    /// Asked of the child order, not by hit-testing: a headless check's editor is on no desktop.
    bool footerIsInFrontOfSettings();

protected:
    /// What the plugin built before this, and hands over. Every one of them outlives this class.
    struct Parts {
        PatchControls& controls;
        ui::InstanceModel& instances;  ///< what the header's tabs list: an effect's own kind, the encoder's scene
        ui::ParameterPage& page;
        juce::Component& globe;
        juce::Component& equirect;
        /// How the scene is looked at; the window reads `equirectFull` from it when it lays the scene row out.
        ui::SceneViewState& view;
    };

    /// `levels` and `footerSwitches` are the plugin's own parameters, shown over the output meter and in the footer.
    PluginEditor(host::PluginProcessor& processor, Parts parts, const juce::String& plugin,
                 std::vector<ui::LevelColumn::Level> levels, std::vector<ui::StatusFooter::Switch> footerSwitches);

    //  ---- what a plugin answers ---------------------------------------------------------------
    /// One frame: whatever of the plugin's own moves, then `frame.refresh(...)` with what it knows that the parts do not.
    virtual void frame(ui::EditorFrame& frame) = 0;
    /// The controls changed, or the energy picture moved: repaint whatever of the plugin's own shows them.
    virtual void controlsChanged() {}
    /// What this plugin's settings page has beside the suite's: nothing, unless it says so.
    virtual void addOwnSettings(ui::SettingsContent&) {}
    /// The settings page is opening or closing: a plugin with a page of its own over the window hides it.
    virtual void settingsShowing(bool) {}
    /// A control on the panel that is not a tile, a level or a footer switch. Painted already.
    virtual bool drawnOnPanel(ParamId) { return false; }
    /// What the footer's order is: the plugin's processor knows.
    virtual int renderedOrder() const = 0;

    //  ---- for a plugin's own code, after construction -----------------------------------------
    /// Lay the scene row out again: `full` changed, and the globe is hidden or back.
    void relayout();
    /// A page of the plugin's own that covers what the settings page covers. Hidden until shown.
    void addOverlay(juce::Component& overlay);
    /// Bring the footer in front of whatever covers the window, as the settings page has it.
    void footerToFront();
    void showSettings(bool open);
    ui::Header& header() noexcept { return header_; }
    ui::MatrixView& matrix() noexcept { return matrix_; }

private:
    /// Private, all of it: a derived constructor's initialiser list looks up a bare name in this class first.
    void timerCallback() override { tick(); }
    void presetChanged();
    ui::SettingsContent settingsContent();
    /// Where a parameter's control is: a tile, a level or a footer switch. Scrolls a tile into view first. Null if nowhere on screen.
    ui::HitArea* controlOf(ParamId id, juce::Rectangle<float>& box);

    /// The window at its designed size; the editor scales it.
    struct Shell final : juce::Component {
        explicit Shell(PluginEditor& o) : owner(o) { setOpaque(true); }
        void paint(juce::Graphics& g) override;
        /// The plugin's own rule over the footer row, the window's whole width, above everything.
        void paintOverChildren(juce::Graphics& g) override;
        void resized() override;
        PluginEditor& owner;
    };

    host::PluginProcessor& processor_;
    PatchControls& controls_;
    ui::ParameterPage& page_;
    juce::Component& globe_;
    juce::Component& equirect_;
    int shownTab_{0}, shownSource_{-1};  ///< what the page last showed, to reset its scroll
    ui::SceneViewState& view_;
    juce::Component* overlay_{nullptr};
    std::vector<float> energyLeaving_, energyArrived_;  ///< the newest covariances, from the processor or the bus
    Uuid energyOf_;                                     ///< whose picture the field holds; nil is this instance's
    ui::Header header_;
    ui::MatrixView matrix_;
    ui::LevelColumn levels_;     ///< what leaves the plugin, over the output meter
    ui::StatusFooter footer_;    ///< the instance-wide switches and the readout
    ui::SettingsPage settings_;  ///< the suite's frame, and this plugin's own beside it
    ProcessorPresets presets_;   ///< before the browser, which asks it where to lay its name row
    ui::PresetBrowser browser_;  ///< over the panel
    ui::ConfirmBox confirm_;     ///< over the whole window: the one thing that cannot be undone
    Shell shell_;
    ui::Tooltips tooltips_{*this};  ///< last, so it sits above every other child
};

}  // namespace bambi::editor
