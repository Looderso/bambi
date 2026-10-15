// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi live host: the app that plays a Live set's tracks through the suite's own plugins. It opens the loop
//  device, plays every track on a pair through an Encoder of its own and the Decoder, answers the bambi Live device and gives
//  the plugins Live's transport, and each Encoder its track's name and colour. The plugins' states are kept with
//  the set, through the device, and given back when the set returns them. The window shows one plugin at a time,
//  chosen above it.

#include <array>
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

//  A track's name and colour as a plugin takes them; nothing for a pair no track is on.
juce::AudioProcessor::TrackProperties propertiesOf(const bambi::live::Node* track) {
    juce::AudioProcessor::TrackProperties p;
    if (track == nullptr) return p;
    p.name = juce::String(track->name);
    if (track->colour.size() == 7 && track->colour[0] == '#')
        p.colourARGB = 0xff000000u | static_cast<juce::uint32>(juce::String(track->colour.substr(1)).getHexValue32());
    return p;
}

class Content final : public juce::Component, private juce::Timer {
public:
    Content() : device_({kPairs, bambi::loop::installed(), 0.0, 48000.0}) {
        //  The port first: the device starts the stand-in host when nothing answers there, and loading the
        //  plugins takes seconds.
        device_.transport = [this](const bambi::live::TransportMessage& m, std::int64_t stamp) {
            engine_.transport(m, stamp);
        };
        device_.changed = [this] { update(); };
        device_.restored = [this] {
            if (const bambi::Json* states = device_.session().kept("plugins")) {
                engine_.restore(*states);
                kept_ = states->dump(0);
            }
        };
        device_.show = [this] {
            if (auto* top = getTopLevelComponent()) top->toFront(true);
            juce::Process::makeForegroundProcess();
        };
        listening_ = device_.start(&error_);
        juce::String status;
        engine_.start({3, kPairs, 64, 48000.0, 0}, status);
        started_ = status;
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
        startTimer(1000);
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

    //  The plugins' states, kept with the set when they changed. Only while bambi is on: a device sends what the set
    //  kept before it switches on, and the defaults must not be kept over it.
    void timerCallback() override {
        engine_.tidy();
        if (!device_.session().on()) return;
        bambi::Json states = engine_.states();
        std::string text = states.dump(0);
        if (text == kept_) return;
        kept_ = std::move(text);
        device_.keep("plugins", std::move(states));
    }

    void update() {
        live_.setText(describe(device_.session(), listening_, error_), juce::dontSendNotification);
        if (const auto& pairs = device_.session().pairs(); pairs != onPairs_) {
            onPairs_ = pairs;
            juce::String status;
            if (engine_.setTracks(pairs, status)) {
                named_.fill({});
                device_.setLatency(engine_.latencyMs(), 48000.0);
                //  a window on an Encoder let go of closes before that Encoder can be freed
                const int shown = choice_.getSelectedId();
                if (shown != kDecoderId && engine_.encoder(shown - 2) != shownPlugin_) showEditor(kDecoderId);
            }
            if (status.isNotEmpty()) status_.setText(started_ + " · " + status, juce::dontSendNotification);
        }
        for (int pair = 0; pair < kPairs; ++pair) {
            const auto* track = device_.session().trackOn(pair);
            const juce::String now = track == nullptr ? juce::String() : juce::String(track->name + track->colour);
            if (now == named_[static_cast<std::size_t>(pair)]) continue;
            named_[static_cast<std::size_t>(pair)] = now;
            if (auto* encoder = engine_.encoder(pair)) encoder->updateTrackProperties(propertiesOf(track));
        }
        const int selected = choice_.getSelectedId();
        choice_.clear(juce::dontSendNotification);
        choice_.addItem("Decoder", kDecoderId);
        for (int pair = 0; pair < kPairs; ++pair)
            if (engine_.encoder(pair) != nullptr) {
                const auto* track = device_.session().trackOn(pair);
                choice_.addItem("Encoder, pair " + juce::String(pair + 1) +
                                    (track == nullptr ? "" : " -- " + juce::String(track->name)),
                                2 + pair);
            }
        choice_.setSelectedId(selected, juce::dontSendNotification);
    }

    void showEditor(int id) {
        if (editor_ != nullptr) removeChildComponent(editor_.get());
        editor_.reset();
        auto* plugin = id == kDecoderId ? engine_.decoder() : engine_.encoder(id - 2);
        shownPlugin_ = plugin;
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
    juce::String started_;                         ///< what the engine said when it started
    std::vector<bambi::live::TrackPair> onPairs_;  ///< the pairs the Encoders were last loaded for
    juce::AudioProcessor* shownPlugin_{nullptr};   ///< the instance the window shows
    std::string kept_;                             ///< the plugins' states as last kept or restored
    std::array<juce::String, kPairs> named_{};     ///< each Encoder's track name and colour as last given
    bambi::livehost::DeviceServer device_;         ///< last: gone first, before what its events touch
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
