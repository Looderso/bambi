// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

#include <array>

#include "bambi/ui/SettingsPage.h"

namespace {}  // namespace

BambiDecoderEditor::BambiDecoderEditor(BambiDecoderProcessor& p)
    : bambi::ui::DecoderWindowParts(p),
      bambi::editor::PluginEditor(p, {controls, controls, panel, globe, equirect, controls.sphere, &strip}, "decoder",
                                  {{bambi::decoderParam("output.gain"), "gain"}}, {}),
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
    //  the session's instances, for the sources the scene draws -- and its other decoders, which this window can
    //  show and edit instead of its own: the header's switch, offered only while there is another
    processor_.updateScene(controls.scene);
    controls.liveScene = &controls.scene;
    controls.refreshSourceRows();
    const bambi::Uuid self = processor_.identity().instance;
    controls.decoders.clear();
    controls.decoders.emplace_back(self, "this");
    for (const auto& entry : controls.scene.entries())
        if (entry.product == bambi::Product::Decoder && !(entry.instance == self))
            controls.decoders.emplace_back(entry.instance, juce::String(entry.label()));
    const bool other = !(controls.shownDecoder == self) && !(controls.shownDecoder == bambi::Uuid{});
    const auto* peer = other ? controls.scene.find(controls.shownDecoder) : nullptr;
    if (peer != nullptr && peer->product != bambi::Product::Decoder) peer = nullptr;
    if (other && peer == nullptr) controls.shownDecoder = self;  // it left the session: back to this one
    const bool remote = peer != nullptr;
    const bool switched =
        remote != controls.remote || (remote && !(peer->instance == controls.remoteTarget.instance()));
    if (switched) controls.remoteTarget.release();
    controls.remote = remote;
    if (remote) controls.remoteTarget.follow(*peer, 1.0 / bambi::ui::kFrameHz);
    controls.shownInstance = remote ? peer->instance : self;
    controls.selectedLabel = remote ? juce::String(peer->label()) : juce::String();
    std::vector<juce::String> labels;
    int selected = 0;
    for (std::size_t i = 0; i < controls.decoders.size(); ++i) {
        labels.push_back(controls.decoders[i].second);
        if (controls.decoders[i].first == controls.shownInstance) selected = static_cast<int>(i);
    }
    header().setInstanceSwitch(std::move(labels), selected, [this](int index) {
        if (index < 0 || index >= static_cast<int>(controls.decoders.size())) return;
        controls.shownDecoder = controls.decoders[static_cast<std::size_t>(index)].first;
        controls.notify();
    });
    const bool changed = controls.refresh() || switched;

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

    //  a meter per channel of the bus: what each output writes is read off the column, and the channel map says whose
    std::array<float, bambi::kMaxChannels> peaks{};
    const int metered =
        std::min(diag.outputChannels.load(std::memory_order_relaxed), static_cast<int>(bambi::kMaxChannels));
    for (int c = 0; c < metered; ++c)
        peaks[static_cast<std::size_t>(c)] =
            diag.channelPeak[static_cast<std::size_t>(c)].load(std::memory_order_relaxed);

    //  learn mode's list lives: the tracking tab is repainted twice a second while it is open on an OSC source
    const bool trackingOpen =
        controls.shownTab > 0 && controls.shownTab <= static_cast<int>(controls.temps.size()) &&
        controls.temps[static_cast<std::size_t>(controls.shownTab - 1)] == bambi::ui::TempTab::Tracking;
    if (trackingOpen && controls.decoder().trackerSource == 1 && ++listFrames_ % (bambi::ui::kFrameHz / 2) == 0)
        panel.repaint();

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
                                      processor_.dspLoad(),
                                      peaks.data(),
                                      metered});
    //  the head moves every frame, whoever moves it, and so does what the scene draws of it
    globe.repaint();
    equirect.repaint();
    strip.repaint();
    const bool hostMoved = processor_.hostOutputs() != hostShown_;
    if (hostMoved) hostShown_ = processor_.hostOutputs();
    if (changed || moved || hostMoved) panel.repaint();
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

bool BambiDecoderEditor::clickGlobeAt(juce::Point<float> at) {
    paintNow();
    globe.clickAt(at);
    return true;
}

bool BambiDecoderEditor::clickSourcesToggle() {
    paintNow();
    const auto area = globe.sourcesToggle().toggle;
    if (area.isEmpty()) return false;
    globe.clickAt(area.getCentre());
    return true;
}

bool BambiDecoderEditor::clickSourcesTriangle() {
    paintNow();
    const auto area = globe.sourcesToggle().triangle;
    if (area.isEmpty()) return false;
    globe.clickAt(area.getCentre());
    return true;
}

juce::Rectangle<float> BambiDecoderEditor::sourcesRowArea(int index) const {
    const auto& rows = controls.sources.listAreas().rows;
    return index >= 0 && index < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(index)]
                                                               : juce::Rectangle<float>{};
}

bool BambiDecoderEditor::clickSourcesRow(int index) {
    paintNow();
    const auto area = sourcesRowArea(index);
    if (area.isEmpty()) return false;
    globe.clickAt(area.getCentre());
    return true;
}

bool BambiDecoderEditor::clickSourcesAll(bool on) {
    paintNow();
    const auto area = on ? controls.sources.listAreas().all : controls.sources.listAreas().none;
    if (area.isEmpty()) return false;
    globe.clickAt(area.getCentre());
    return true;
}

bool BambiDecoderEditor::dragGlobeBetween(juce::Point<float> from, juce::Point<float> to) {
    paintNow();
    globe.dragBetween(from, to);
    return true;
}
