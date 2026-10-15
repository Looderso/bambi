// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi live host: the app that plays a Live set's tracks through the suite's own plugins. It opens the loop
//  device, plays every pair through an Encoder of its own and the Decoder, answers the bambi Live device and gives
//  the plugins Live's transport. The window shows one plugin at a time, chosen above it.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Device.h"
#include "Engine.h"
#include "loop.hpp"

namespace {

constexpr int kPairs = 7;  ///< the loop device's 16 channels: 7 pairs for tracks, the eighth for the return

//  What the device has said, in one line.
juce::String describe(const bambi::live::DeviceSession& s, bool listening, const std::string& error) {
    if (!listening)
        return "Live: cannot listen on 127.0.0.1:" + juce::String(bambi::livehost::DeviceServer::kPort) + " (" +
               juce::String(error) + ") -- is the stand-in host running?";
    if (!s.connected()) return "Live: waiting for the bambi device";
    juce::String line = "Live " + juce::String(s.hello()->live) + ": bambi " + (s.on() ? "on" : "off") + ", " +
                        juce::String(static_cast<int>(s.pairs().size())) + " tracks on pairs";
    if (!s.unassigned().empty()) line << ", " << static_cast<int>(s.unassigned().size()) << " without one";
    if (s.transport().has_value())
        line << (s.transport()->playing ? ", playing at " : ", stopped at ") << juce::String(s.transport()->ppq, 2);
    return line;
}

//  The track on a pair, by its name in Live; empty when none is.
juce::String trackOn(const bambi::live::DeviceSession& s, int pair) {
    for (const auto& p : s.pairs())
        if (p.pair == pair)
            for (const auto& n : s.tree().nodes)
                if (n.id == p.track) return n.name;
    return {};
}

class Content final : public juce::Component {
public:
    Content() : device_({kPairs, bambi::loop::installed(), 0.0, 48000.0}) {
        //  The port first: the device starts the stand-in host when nothing answers there, and loading the
        //  plugins takes seconds.
        device_.transport = [this](const bambi::live::TransportMessage& m, std::int64_t stamp) {
            engine_.transport(m, stamp);
        };
        device_.changed = [this] { update(); };
        device_.show = [this] {
            if (auto* top = getTopLevelComponent()) top->toFront(true);
            juce::Process::makeForegroundProcess();
        };
        listening_ = device_.start(&error_);
        juce::String status;
        engine_.start({3, kPairs, 64, 48000.0, 0}, status);
        status_.setText(status, juce::dontSendNotification);
        device_.setLatency(engine_.latencyMs(), 48000.0);

        for (auto* label : {&status_, &live_}) {
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        choice_.onChange = [this] { showEditor(choice_.getSelectedId()); };
        addAndMakeVisible(choice_);
        update();
        choice_.setSelectedId(kDecoderId, juce::dontSendNotification);
        showEditor(kDecoderId);
    }

    ~Content() override { editor_.reset(); }  // the window before the instance it shows

    void resized() override {
        auto area = getLocalBounds();
        status_.setBounds(area.removeFromTop(kRow).reduced(8, 0));
        live_.setBounds(area.removeFromTop(kRow).reduced(8, 0));
        choice_.setBounds(area.removeFromTop(kRow).reduced(8, 2).withWidth(320));
        if (editor_ != nullptr) editor_->setBounds(area.withSize(editor_->getWidth(), editor_->getHeight()));
    }

private:
    static constexpr int kRow = 24, kDecoderId = 1;  // a pair's id is 2 + the pair

    void update() {
        live_.setText(describe(device_.session(), listening_, error_), juce::dontSendNotification);
        const int selected = choice_.getSelectedId();
        choice_.clear(juce::dontSendNotification);
        choice_.addItem("Decoder", kDecoderId);
        for (int pair = 0; pair < kPairs; ++pair) {
            const juce::String track = trackOn(device_.session(), pair);
            choice_.addItem("Encoder, pair " + juce::String(pair + 1) + (track.isEmpty() ? "" : " -- " + track),
                            2 + pair);
        }
        choice_.setSelectedId(selected, juce::dontSendNotification);
    }

    void showEditor(int id) {
        if (editor_ != nullptr) removeChildComponent(editor_.get());
        editor_.reset();
        auto* plugin = id == kDecoderId ? engine_.decoder() : engine_.encoder(id - 2);
        if (plugin != nullptr) editor_.reset(plugin->createEditorAndMakeActive());
        if (editor_ != nullptr) addAndMakeVisible(*editor_);
        const int w = std::max(640, editor_ != nullptr ? editor_->getWidth() : 0);
        setSize(w, (editor_ != nullptr ? editor_->getHeight() : 120) + 3 * kRow);
        resized();
    }

    bambi::livehost::Engine engine_;
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    juce::Label status_, live_;
    juce::ComboBox choice_;
    bool listening_{false};
    std::string error_;
    bambi::livehost::DeviceServer device_;  ///< last: gone first, before what its events touch
};

class Window final : public juce::DocumentWindow {
public:
    Window()
        : DocumentWindow("bambi live host", juce::Colours::black,
                         DocumentWindow::closeButton | DocumentWindow::minimiseButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(new Content(), true);
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "bambi live host"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override { window_ = std::make_unique<Window>(); }
    void shutdown() override { window_.reset(); }

private:
    std::unique_ptr<Window> window_;
};

}  // namespace

START_JUCE_APPLICATION(App)
