// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi live host: the app that plays a Live set's tracks through the suite's own plugins. It opens the loop
//  device, plays every track on a pair through an Encoder of its own and the Decoder, answers the bambi Live device
//  and gives the plugins Live's transport, and each Encoder its track's name and colour. The plugins' states are kept with
//  the set, through the device, and given back when the set returns them. The window is the host's column -- the
//  views to switch between, and what it hears from Live -- beside the plugin the view shows, whole, with its own
//  header and tabs: the encoder view one track's Encoder, whose tabs reach the others.

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Device.h"
#include "Engine.h"
#include "Rail.h"
#include "bambi/ui/Theme.h"
#include "loop.hpp"

namespace {

constexpr int kPairs = 7;  ///< the loop device's 16 channels: 7 pairs for tracks, the eighth for the return

//  What the host hears from Live, in a few short lines, and what went wrong if something did.
bambi::livehost::Rail::Status describe(const bambi::live::DeviceSession& s, bool listening, const std::string& error,
                                       const juce::String& problem, double latencyMs) {
    if (!listening)
        return {false,
                {"live", "port taken"},
                "port " + juce::String(bambi::livehost::DeviceServer::kPort) + ": " + juce::String(error) +
                    ". Is the stand-in host running?"};
    if (!s.connected()) return {false, {"live", "waiting"}, problem};
    const int sources = static_cast<int>(s.pairs().size());
    return {true,
            {s.on() ? "live" : "live, off", juce::String(sources) + (sources == 1 ? " source" : " sources"),
             juce::String(latencyMs, 1) + " ms"},
            problem};
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
        running_ = engine_.start({3, kPairs, 64, 48000.0, 0}, started_);
        device_.setLatency(engine_.latencyMs(), 48000.0);

        addAndMakeVisible(rail_);
        update();
        showView(View::Decoder);
        startTimer(1000);
    }

    ~Content() override { editor_.reset(); }  // the window before the instance it shows

    void paint(juce::Graphics& g) override { g.fillAll(bambi::ui::theme::colour::ground); }

    void resized() override {
        auto area = getLocalBounds();
        rail_.setBounds(area.removeFromLeft(bambi::ui::theme::rail::width));
        if (editor_ != nullptr) editor_->setBounds(area.withSize(editor_->getWidth(), editor_->getHeight()));
    }

private:
    enum class View { Encoder, Echo, Reverb, Decoder };

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
        const auto& session = device_.session();
        if (session.pairs() != onPairs_ || session.field().parts != onParts_) {
            onPairs_ = session.pairs();
            onParts_ = session.field().parts;
            juce::String status;
            const bool changed = engine_.setField(session.field(), session.pairs(), status);
            problem_ = changed || status.isEmpty() ? juce::String() : status;
            if (changed) {
                named_.fill({});
                device_.setLatency(engine_.latencyMs(), 48000.0);
                //  a window on an Encoder let go of closes before that Encoder can be freed
                if (view_ != View::Decoder && !engine_.holds(shownPlugin_)) showView(view_);
            }
        }
        engine_.setSends(session.field(), session.tree());
        for (int pair = 0; pair < kPairs; ++pair) {
            const auto* track = device_.session().trackOn(pair);
            const juce::String now = track == nullptr ? juce::String() : juce::String(track->name + track->colour);
            if (now == named_[static_cast<std::size_t>(pair)]) continue;
            named_[static_cast<std::size_t>(pair)] = now;
            if (auto* encoder = engine_.encoder(pair)) encoder->updateTrackProperties(propertiesOf(track));
        }
        //  the views there is something to show in: a kind of plugin once one plays
        views_.clear();
        for (const View v : {View::Encoder, View::Echo, View::Reverb, View::Decoder})
            if (pluginOf(v) != nullptr) views_.push_back(v);
        std::vector<juce::String> labels;
        int selected = 0;
        for (std::size_t i = 0; i < views_.size(); ++i) {
            labels.push_back(nameOf(views_[i]));
            if (views_[i] == view_) selected = static_cast<int>(i);
        }
        rail_.setViews(labels, selected, [this](int i) { showView(views_[static_cast<std::size_t>(i)]); });
        rail_.setStatus(running_ ? describe(device_.session(), listening_, error_, problem_, engine_.latencyMs())
                                 : bambi::livehost::Rail::Status{false, {"audio", "off"}, started_});
    }

    juce::AudioProcessor* pluginOf(View v) const {
        switch (v) {
            case View::Encoder: return engine_.firstEncoder();
            case View::Echo: return engine_.firstEffect(bambi::live::Effect::Echo);
            case View::Reverb: return engine_.firstEffect(bambi::live::Effect::Reverb);
            case View::Decoder: return engine_.decoder();
        }
        return nullptr;
    }

    static const char* nameOf(View v) {
        switch (v) {
            case View::Encoder: return "encoder";
            case View::Echo: return "echo";
            case View::Reverb: return "reverb";
            case View::Decoder: return "decoder";
        }
        return "";
    }

    void showView(View v) {
        if (editor_ != nullptr) removeChildComponent(editor_.get());
        editor_.reset();
        if (pluginOf(v) == nullptr) v = View::Decoder;
        auto* plugin = pluginOf(v);
        view_ = v;
        shownPlugin_ = plugin;
        if (plugin != nullptr) editor_.reset(plugin->createEditorAndMakeActive());
        if (editor_ != nullptr) addAndMakeVisible(*editor_);
        const int w = editor_ != nullptr ? editor_->getWidth() : bambi::ui::theme::metrics::windowWidth;
        const int h = editor_ != nullptr ? editor_->getHeight() : bambi::ui::theme::metrics::windowHeight;
        setSize(bambi::ui::theme::rail::width + w, h);
        resized();
        update();
    }

    bambi::livehost::Engine engine_;
    bool running_{false};
    juce::String started_;  ///< what the engine said when it started, or why it did not
    juce::String problem_;  ///< why the last change to what it plays could not be made
    bambi::livehost::Rail rail_;
    std::vector<View> views_;
    View view_{View::Decoder};
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    bool listening_{false};
    std::string error_;                            ///< why the port could not be opened
    std::vector<bambi::live::TrackPair> onPairs_;  ///< the pairs and the field the program was last built for
    std::vector<bambi::live::Part> onParts_;
    juce::AudioProcessor* shownPlugin_{nullptr};  ///< the instance the window shows
    std::string kept_;                            ///< the plugins' states as last kept or restored
    std::array<juce::String, kPairs> named_{};    ///< each Encoder's track name and colour as last given
    bambi::livehost::DeviceServer device_;        ///< last: gone first, before what its events touch
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
