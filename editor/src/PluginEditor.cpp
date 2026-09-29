// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/editor/PluginEditor.h"

#include <algorithm>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::editor {

namespace {
namespace colour = ui::theme::colour;
namespace m = ui::theme::metrics;

/// A press and a release on the header, where the mouse would make them: it is not a `HitArea`.
void clickHeader(ui::Header& header, juce::Point<float> at) {
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, &header, &header, now, at, now, 1, false);
    header.mouseDown(e);
    header.mouseUp(e);
}

/// The rule between the scene row and the matrix.
int matrixTop() { return m::headerHeight + m::sceneTop + m::scenePanel + ui::theme::space::gap; }
}  // namespace

PluginEditor::PluginEditor(host::PluginProcessor& processor, Parts parts, const juce::String& plugin,
                           std::vector<ui::LevelColumn::Level> levels,
                           std::vector<ui::StatusFooter::Switch> footerSwitches)
    : juce::AudioProcessorEditor(&processor),
      processor_(processor),
      controls_(parts.controls),
      page_(parts.page),
      globe_(parts.globe),
      equirect_(parts.equirect),
      view_(parts.view),
      header_(
          parts.instances, plugin,
          [this] { showSettings(!settings_.isVisible() && (overlay_ == nullptr || !overlay_->isVisible())); },
          [this] {
              controls_.target().undo();
              controls_.refresh();
              controls_.notify();
          },
          [this] {
              controls_.target().redo();
              controls_.refresh();
              controls_.notify();
          }),
      matrix_(controls_),
      levels_(controls_, std::move(levels)),
      footer_(controls_, std::move(footerSwitches)),
      settings_(
          plugin, [this] { return settingsContent(); },
          [this](const juce::String& name) { processor_.linkNode().rename(name.toStdString()); },
          [this] { showSettings(false); }),
      presets_(processor, parts.controls),
      browser_(
          presets_, [this] { presetChanged(); }, [this] { openPresets(false); },
          [this](juce::String question, juce::String note, juce::String yes, std::function<void()> confirmed) {
              confirm_.ask(std::move(question), std::move(note), std::move(yes), std::move(confirmed));
          }),
      shell_(*this) {
    confirm_.answered = [this] { browser_.takeKeyboard(); };
    //  home: back to this window's own instance, from whichever it is showing
    header_.onHome([this, &instances = parts.instances] { instances.selectInstance(processor_.identity().instance); });
    //  which plugin this is: its own colour, on the header's rule and the footer's and on what modulates
    colour::setAccent(processor_.linkHost().product());
    header_.setIdentity(colour::identity(processor_.linkHost().product()));
    header_.onPreset([this] { openPresets(!browser_.isVisible()); },
                     [this](int delta) {
                         if (presets_.step(delta)) presetChanged();
                     });
    controls_.changed = [this] {
        /*  Another page starts at its top. The scroll is the panel's, not the page's, so a regions
            tab scrolled down opened the room tab scrolled past its own buttons. */
        if (controls_.tab != shownTab_ || controls_.source != shownSource_) {
            shownTab_ = controls_.tab;
            shownSource_ = controls_.source;
            page_.scrollTo(0.0f);
        }
        matrix_.repaint();
        page_.repaint();
        levels_.repaint();
        footer_.repaint();
        header_.setUndoState(controls_.target().canUndo(), controls_.target().canRedo(),
                             juce::String(controls_.target().undoName()), juce::String(controls_.target().redoName()));
        controlsChanged();
    };
    const auto link = processor_.linkStatus();
    header_.setStatus({link.open, link.session, link.peers});

    for (auto* child :
         std::initializer_list<juce::Component*>{&header_, &globe_, &equirect_, &matrix_, &page_, &levels_, &footer_})
        shell_.addAndMakeVisible(child);
    shell_.addChildComponent(settings_);
    shell_.addChildComponent(browser_);
    shell_.addChildComponent(confirm_);
    addAndMakeVisible(shell_);

    //  Read before the limits: setting them clamps an unsized window to the smallest, and `resized`
    //  records that.
    const float scale = processor_.windowScale();
    //  Designed at one size, scaled uniformly. The host resizes the window; JUCE's own corner grip
    //  would draw in its look, not this one.
    setResizable(true, false);
    const auto sized = [](int design, float by) { return juce::roundToInt(static_cast<float>(design) * by); };
    setResizeLimits(sized(m::windowWidth, kWindowScaleMin), sized(m::windowHeight, kWindowScaleMin),
                    sized(m::windowWidth, kWindowScaleMax), sized(m::windowHeight, kWindowScaleMax));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(m::windowWidth) / static_cast<double>(m::windowHeight));
    //  As the window was left: the view while the plugin is loaded, the size from the session.
    if (const auto& kept = processor_.viewMemory(); kept.kept) {
        view_.camera = kept.camera;
        view_.preset = kept.preset;
        view_.regionsAlways = kept.regionsAlways;
        view_.equirectFull = kept.equirectFull;
    }
    setSize(sized(m::windowWidth, scale), sized(m::windowHeight, scale));

    processor_.noteUserActivity();  // opening an editor is where the user is working
    startTimerHz(ui::kFrameHz);
}

PluginEditor::~PluginEditor() {
    stopTimer();
    processor_.viewMemory() = {true, view_.camera, view_.preset, view_.regionsAlways, view_.equirectFull};
    controls_.changed = nullptr;
    processor_.wantEnergy(false);  // nobody is looking any more: the audio thread stops gathering
    /*  Nobody is left to watch an envelope that is listening for a note. A gesture still open on
        another instance needs nothing here: the control state is destroyed with the window, and
        `RemoteTarget` ends what it began in its own destructor. */
    controls_.stopLearning();
}

void PluginEditor::paint(juce::Graphics& g) { g.fillAll(colour::ground); }

void PluginEditor::Shell::paintOverChildren(juce::Graphics& g) {
    //  over the matrix's footer, the page's footer and the level column alike: one rule, the window's own
    g.setColour(colour::identity(owner.processor_.linkHost().product()));
    g.fillRect(0.0f, static_cast<float>(m::windowHeight - m::footerHeight), static_cast<float>(m::windowWidth),
               ui::theme::stroke::identityRule);
}

void PluginEditor::resized() {
    //  the smaller of the two ratios: a host that ignores the aspect constrainer letterboxes the
    //  window rather than cropping its footer
    const float scale = std::min(static_cast<float>(getWidth()) / static_cast<float>(m::windowWidth),
                                 static_cast<float>(getHeight()) / static_cast<float>(m::windowHeight));
    shell_.setTransform(juce::AffineTransform::scale(scale));
    processor_.setWindowScale(scale);
    shell_.setBounds(0, 0, m::windowWidth, m::windowHeight);
}

void PluginEditor::relayout() { shell_.resized(); }

void PluginEditor::Shell::resized() {
    auto& o = owner;
    //  The two views sit side by side, both square -- or the equirect has the whole row and the globe
    //  is hidden.
    const int top = m::headerHeight + m::sceneTop;
    o.header_.setBounds(0, 0, m::windowWidth, m::headerHeight);
    const auto row = ui::sceneRow(ui::theme::space::inset, top, o.view_.equirectFull);
    o.globe_.setVisible(!row.globe.isEmpty());
    if (!row.globe.isEmpty()) o.globe_.setBounds(row.globe);
    o.equirect_.setBounds(row.equirect);
    o.matrix_.setBounds(0, matrixTop() + 1, m::leftColumn, m::windowHeight - matrixTop() - 1);
    /*  The right-hand side: the panel between two rules, the footer under it, and the level
        column down the window's edge. */
    const int rightX = m::leftColumn + 1;
    const int rightWidth = m::windowWidth - m::meterColumn - rightX;
    o.page_.setBounds(rightX, m::headerHeight, rightWidth, m::windowHeight - m::headerHeight - m::footerHeight);
    o.browser_.setBounds(o.page_.getBounds());  // over the panel, as a source's page is
    o.confirm_.setBounds(0, 0, m::windowWidth, m::windowHeight);
    o.footer_.setBounds(rightX, m::windowHeight - m::footerHeight, rightWidth, m::footerHeight);
    o.levels_.setBounds(m::windowWidth - m::meterColumn, m::headerHeight, m::meterColumn,
                        m::windowHeight - m::headerHeight);
    //  the settings page, and a plugin's own page, cover everything but the column -- and the footer
    //  comes in front of them
    const juce::Rectangle<int> cover{0, m::headerHeight, m::windowWidth - m::meterColumn,
                                     m::windowHeight - m::headerHeight};
    o.settings_.setBounds(cover);
    if (o.overlay_ != nullptr) o.overlay_->setBounds(cover);
}

void PluginEditor::Shell::paint(juce::Graphics& g) {
    g.fillAll(colour::ground);
    /*  The header draws its own band, rule and tabs. The rule under it is not drawn here: the
        selected tab opens through it, so whoever draws the tabs draws it. */
    const auto headerBottom = static_cast<float>(m::headerHeight);
    const auto height = static_cast<float>(m::windowHeight);
    ui::ruleH(g, 0.0f, static_cast<float>(m::leftColumn), static_cast<float>(matrixTop()), colour::ruleStrong);
    //  the two rules that frame the right-hand panel
    ui::ruleV(g, static_cast<float>(m::leftColumn), headerBottom, height, colour::ruleStrong);
    ui::ruleV(g, static_cast<float>(m::windowWidth - m::meterColumn), headerBottom, height, colour::ruleStrong);
}

void PluginEditor::addOverlay(juce::Component& overlay) {
    overlay_ = &overlay;
    shell_.addChildComponent(overlay);
    shell_.resized();
}

void PluginEditor::footerToFront() { footer_.toFront(false); }

/*  The page covers the scene, the matrix and the tabs. The footer comes to the front of it and the
    level column is beside it: what is always on screen stays on screen. */
void PluginEditor::showSettings(bool open) {
    if (open) openPresets(false);  // one page at a time
    settingsShowing(open);
    settings_.open(open, footer_);
    header_.setSettingsOpen(open);
}

ui::SettingsContent PluginEditor::settingsContent() {
    auto c = ui::sharedSettings(processor_, renderedOrder());
    addOwnSettings(c);
    ui::addHostReadout(c, processor_.readout());  // the same three rows in every plugin
    return c;
}

void PluginEditor::tick() {
    ui::EditorFrame parts{header_, matrix_, page_, settings_, levels_, footer_};
    frame(parts);

    /*  The energy picture, the same in every plugin: of the instance the window shows. Its own is
        told whether anybody is looking, every frame, and gathers only while somebody is; another
        instance is asked over the bus, and sends its summary cut to order 3 while it is being asked.
        The expensive half runs only when a covariance lands, about twenty times a second; the
        ballistics and the colour run every frame, so the smoothing has frames to work with. */
    view_.energyAvailable = true;
    const bool looking = view_.energyOn;
    processor_.wantEnergy(looking && !controls_.remote);
    controls_.remoteTarget.wantEnergy(looking && controls_.remote);
    //  another instance's picture is not this one's: the field starts empty when the window moves
    const Uuid shows = controls_.remote ? controls_.remoteTarget.instance() : Uuid{};
    if (!(shows == energyOf_)) {
        energyOf_ = shows;
        view_.energy.clear();
    }
    if (looking) {
        int order = processor_.engineOrder();
        const bool landed = controls_.remote ? controls_.remoteTarget.takeEnergy(energyLeaving_, energyArrived_, order)
                                             : processor_.takeCovariances(energyLeaving_, energyArrived_);
        if (order >= 0 && view_.energy.order() != order) view_.energy.prepare(order);
        if (landed) view_.energy.sample(energyLeaving_, energyArrived_);
        if (view_.energy.step(1.0 / ui::kFrameHz)) controlsChanged();
    }
    //  the header's preset cluster, and the browser under it
    const auto preset = presets_.current();
    header_.setPreset({juce::String(preset.name), presets_.modified(), browser_.isVisible(), presets_.available()});
    browser_.refresh();

    //  lit while either page covers the window: a glyph that looks closed must not close something
    header_.setSettingsOpen(settings_.isVisible() || overlayShown());
}

// ---- presets --------------------------------------------------------------------------

void PluginEditor::openPresets(bool open) {
    if (open && (!presets_.available() || settings_.isVisible() || overlayShown())) return;
    browser_.setVisible(open);
    if (open) browser_.toFront(true);
}

/*  A preset was loaded, saved or renamed: the controls read the patch again, as after an undo, and
    the browser's cursor goes to what is loaded -- the header's arrows step while it is open. */
void PluginEditor::presetChanged() {
    controls_.refresh();
    controls_.notify();
    browser_.followCurrent();
}

bool PluginEditor::clickPresetName() {
    paintNow();
    if (!presets_.available() || header_.presetNameArea().isEmpty()) return false;
    clickHeader(header_, header_.presetNameArea().getCentre());
    return true;
}

bool PluginEditor::clickHome() {
    paintNow();
    if (header_.homeArea().isEmpty()) return false;
    clickHeader(header_, header_.homeArea().getCentre());
    return true;
}

bool PluginEditor::clickPresetStep(int delta) {
    paintNow();
    const auto box = header_.presetStepArea(delta > 0);
    if (!presets_.available() || box.isEmpty()) return false;
    clickHeader(header_, box.getCentre());
    return true;
}

bool PluginEditor::clickPresetRow(const PresetRef& ref) {
    if (!browser_.isVisible()) return false;
    paintNow();
    if (browser_.scrollIntoView(browser_.rowArea(ref))) paintNow();
    const auto box = browser_.rowArea(ref);
    if (box.isEmpty()) return false;
    browser_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::clickPresetHeading(const std::string& heading) {
    if (!browser_.isVisible()) return false;
    paintNow();
    if (browser_.scrollIntoView(browser_.headingArea(heading))) paintNow();
    const auto box = browser_.headingArea(heading);
    if (box.isEmpty()) return false;
    browser_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::clickPresetAction(ui::PresetBrowser::Action action) {
    if (!browser_.isVisible()) return false;
    paintNow();
    if (!browser_.actionEnabled(action)) return false;
    browser_.clickAt(browser_.actionArea(action).getCentre());
    return true;
}

bool PluginEditor::clickAnswer(bool yes) {
    if (!confirm_.asking()) return false;
    paintNow();
    confirm_.clickAt((yes ? confirm_.yesArea() : confirm_.noArea()).getCentre());
    return true;
}

void PluginEditor::paintNow() {
    const juce::Image painted = createComponentSnapshot(getLocalBounds(), true, 1.0f);
    juce::ignoreUnused(painted);
}

void PluginEditor::showTab(int index) {
    controls_.showTab(index);
    page_.repaint();
}

ui::HitArea* PluginEditor::controlOf(ParamId id, juce::Rectangle<float>& box) {
    paintNow();
    if (box = levels_.valueArea(id); !box.isEmpty()) return &levels_;
    if (box = footer_.switchArea(id); !box.isEmpty()) return &footer_;
    if (page_.scrollIntoView(page_.nameArea(id))) paintNow();
    box = page_.nameArea(id);
    return box.isEmpty() ? nullptr : &page_;
}

bool PluginEditor::touchParameter(ParamId id) {
    juce::Rectangle<float> box;
    auto* owner = controlOf(id, box);
    if (owner == nullptr) return false;
    owner->clickAt(box.getCentre());
    return true;
}

juce::Rectangle<float> PluginEditor::tileAreaOf(ParamId id) {
    juce::Rectangle<float> box;
    controlOf(id, box);
    return box;
}

void PluginEditor::doubleClickTile(ParamId id) {
    juce::Rectangle<float> box;
    if (auto* owner = controlOf(id, box)) owner->doubleClickAt(box.getCentre());
}

void PluginEditor::dragTile(ParamId id, float dy, float dx) {
    juce::Rectangle<float> box;
    if (auto* owner = controlOf(id, box)) owner->dragBetween(box.getCentre(), box.getCentre().translated(dx, dy));
}

bool PluginEditor::clickTileBar(ParamId id, float share) {
    juce::Rectangle<float> box;
    auto* owner = controlOf(id, box);
    if (owner == nullptr) return false;
    namespace ctl = ui::theme::controls;
    const auto bar = ui::tileBar(box.reduced(ctl::tileBoxPadX, ctl::tileBoxPadY));
    owner->clickAt({bar.getX() + share * bar.getWidth(), bar.getCentreY()});
    return true;
}

bool PluginEditor::drawnSomewhere(ParamId id) {
    paintNow();
    return !page_.drawnArea(id).isEmpty() || !levels_.valueArea(id).isEmpty() || !footer_.switchArea(id).isEmpty() ||
           drawnOnPanel(id);
}

bool PluginEditor::drawnOnPage(ParamId id) {
    paintNow();
    return !page_.drawnArea(id).isEmpty() || drawnOnPanel(id);
}

bool PluginEditor::dragLevel(ParamId id, float dy) {
    paintNow();
    const auto box = levels_.valueArea(id);
    if (box.isEmpty()) return false;
    levels_.dragBetween(box.getCentre(), box.getCentre().translated(0.0f, dy));
    return true;
}

bool PluginEditor::clickLevel(ParamId id) {
    paintNow();
    const auto box = levels_.valueArea(id);
    if (box.isEmpty()) return false;
    levels_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::clickFooterSwitch(ParamId id) {
    paintNow();
    const auto box = footer_.switchArea(id);
    if (box.isEmpty()) return false;
    footer_.clickAt(box.getCentre());
    return true;
}

void PluginEditor::clickSourceColumn(MatrixTab tab, int column) {
    controls_.setCurrentTab(tab);
    matrix_.repaint();
    paintNow();
    /*  Where the head really was, recorded by the paint -- not re-derived here, which would be a
        second copy of the matrix's layout and would drift from it silently. A column the tab does
        not have has no area, and clicking nothing is the honest way to say so. */
    const auto head = matrix_.columnHeadArea(column);
    if (!head.isEmpty()) matrix_.clickAt(head.getCentre());
}

bool PluginEditor::clickMatrixTab(MatrixTab tab) {
    matrix_.repaint();
    paintNow();
    const auto box = matrix_.tabArea(tab);
    if (box.isEmpty()) return false;
    matrix_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::dragEnvelope(EnvelopeGrab grab, float dx, float dy) {
    paintNow();
    const auto from = page_.envelopeGraph().handleAt(grab);
    if (page_.envelopeGraph().plotArea().isEmpty()) return false;
    page_.dragBetween(from, from + juce::Point<float>{dx, dy});
    tick();  // the edit reaches the patch the way every other one does, on a frame
    paintNow();
    return true;
}

bool PluginEditor::clickNamed(const juce::String& name) {
    paintNow();
    if (page_.scrollIntoView(page_.namedArea(name))) paintNow();
    const auto box = page_.namedArea(name);
    if (box.isEmpty()) return false;
    page_.clickAt(box.getCentre());
    return true;
}

void PluginEditor::revealNamed(const juce::String& name) {
    paintNow();
    if (page_.scrollIntoView(page_.namedArea(name).expanded(0.0f, bambi::ui::theme::controls::revealMargin)))
        paintNow();
}

bool PluginEditor::clickSettingsOption(int choice, int option) {
    paintNow();
    const auto box = settings_.optionArea(choice, option);
    if (!settings_.isVisible() || box.isEmpty()) return false;
    settings_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::clickSettingsAction(int index) {
    paintNow();
    const auto box = settings_.actionArea(index);
    if (!settings_.isVisible() || box.isEmpty()) return false;
    settings_.clickAt(box.getCentre());
    return true;
}

bool PluginEditor::clickUndo(bool redo) {
    paintNow();
    const auto box = redo ? header_.redoArea() : header_.undoArea();
    if (box.isEmpty()) return false;
    clickHeader(header_, box.getCentre());
    return true;
}

bool PluginEditor::footerIsInFrontOfOverlay() {
    return overlayShown() && shell_.getIndexOfChildComponent(&footer_) > shell_.getIndexOfChildComponent(overlay_);
}

bool PluginEditor::closeSettingsByItsCross() {
    paintNow();
    if (!settings_.isVisible() || settings_.closeArea().isEmpty()) return false;
    settings_.clickAt(settings_.closeArea().getCentre());
    return true;
}

bool PluginEditor::footerIsInFrontOfSettings() {
    return settings_.isVisible() && settings_.getBounds().intersects(footer_.getBounds()) &&
           shell_.getIndexOfChildComponent(&footer_) > shell_.getIndexOfChildComponent(&settings_);
}

}  // namespace bambi::editor
