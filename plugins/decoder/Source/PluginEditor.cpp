// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

#include "bambi/ui/SettingsPage.h"

namespace {
bambi::ParamId decoderParam(const char* key) { return static_cast<bambi::ParamId>(bambi::decodeParams().byKey(key)); }
}  // namespace

BambiDecoderEditor::BambiDecoderEditor(BambiDecoderProcessor& p)
    : bambi::ui::DecoderWindowParts(p),
      bambi::editor::PluginEditor(p, {controls, controls, panel, globe, equirect, controls.sphere, &strip}, "decoder",
                                  {{decoderParam("output.level"), "level"}}, {}),
      processor_(p) {
    //  the outputs' squares: the one option this plugin asks of the shared header
    header().setTabMarks([this](int index) { return controls.outputMark(index); },
                         [this](int index) { controls.toggleOutput(index); });
    controls.changed = [this] { controlsChanged(); };
    equirect.setRelayout([this] { relayout(); });
    globe.setRelayout([this] { relayout(); });
    tick();
}

BambiDecoderEditor::~BambiDecoderEditor() = default;

void BambiDecoderEditor::frame(bambi::ui::EditorFrame& parts) {
    //  the session's instances, for the sources the scene draws
    processor_.updateScene(controls.scene);
    controls.liveScene = &controls.scene;
    controls.shownInstance = processor_.identity().instance;
    const bool changed = controls.refresh();

    //  the tracker's rate, from the messages counted since the last reading
    const auto& diag = processor_.diagnostics();
    const int messages = diag.trackerMessages.load(std::memory_order_relaxed);
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (now - trackerRateAt_ >= 0.5) {
        trackerRate_ = trackerRateAt_ > 0.0
                           ? static_cast<float>((messages - trackerMessagesSeen_) / (now - trackerRateAt_))
                           : 0.0f;
        trackerMessagesSeen_ = messages;
        trackerRateAt_ = now;
    }
    strip.setTrackerRate(trackerRate_);

    const auto link = processor_.linkStatus();
    const bool moved = parts.refresh({{link.open, link.session, link.peers},
                                      controls.target().canUndo(),
                                      controls.target().canRedo(),
                                      juce::String(controls.target().undoName()),
                                      juce::String(controls.target().redoName()),
                                      changed,
                                      diag.outputPeak.load(std::memory_order_relaxed),
                                      controls.anotherLabel(),
                                      processor_.engineOrder(),
                                      processor_.dspLoad()});
    //  the head moves every frame, whoever moves it, and so does what the scene draws of it
    globe.repaint();
    equirect.repaint();
    strip.repaint();
    if (changed || moved) panel.repaint();
}

void BambiDecoderEditor::controlsChanged() {
    globe.repaint();
    equirect.repaint();
    strip.repaint();
    panel.repaint();
    header().repaint();
}

void BambiDecoderEditor::addOwnSettings(bambi::ui::SettingsContent& c) {
    bambi::ui::addHostReadout(c, processor_.diagnostics());
    const juce::String error = processor_.designError();
    c.readout.push_back(
        {"layout", error.isEmpty()
                       ? juce::String(controls.decoder().layoutName.empty() ? "none" : controls.decoder().layoutName)
                       : "refused: " + error});
    c.readout.push_back({"head model", processor_.modelLoaded() ? juce::String(processor_.headModel().learnedFrom)
                                                                : juce::String("missing")});
    c.readout.push_back(
        {"latency", juce::String(processor_.getLatencySamples()) + " samples, the convolution's partition"});
}

void BambiDecoderEditor::showOutput(int index) {
    controls.selectInstance(bambi::ui::DecoderControlState::outputId(index));
    header().repaint();
    panel.repaint();
}

bool BambiDecoderEditor::clickOutputSquare(int index) {
    paintNow();
    const auto area = header().tabMarkArea(index);
    if (area.isEmpty()) return false;
    header().mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), area.getCentre(), {}, 0.0f,
                                        0.0f, 0.0f, 0.0f, 0.0f, &header(), &header(), juce::Time::getCurrentTime(),
                                        area.getCentre(), juce::Time::getCurrentTime(), 1, false));
    return true;
}
