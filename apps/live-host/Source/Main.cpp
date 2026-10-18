// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi live host: the app that plays a Live set's tracks through the suite's own plugins. It opens the loop
//  device, plays every track on a pair through an Encoder of its own and the Decoder, answers the bambi Live device
//  and gives the plugins Live's transport, and each Encoder its track's name and colour. The plugins' states are kept with
//  the set, through the device, and given back when the set returns them. The window is the host's column -- the
//  views to switch between, and what it hears from Live -- beside the plugin the view shows, whole, with its own
//  header and tabs: the encoder view one track's Encoder, whose tabs reach the others.

#include <algorithm>
#include <array>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioSheet.h"
#include "Device.h"
#include "Engine.h"
#include "Rail.h"
#include "Routing.h"
#include "Settings.h"
#include "bambi/live/midi.hpp"
#include "Timeline.h"
#include "bambi/ui/Theme.h"
#include "devices.hpp"
#include "loop.hpp"

namespace {

constexpr int kPairs = 31;  ///< the most track pairs: bambi's 64 channels less Live's Main's pair

//  The track pairs the loop device has here: its channels less Main's pair, at most kPairs; BlackHole 16ch's 7 when
//  bambi's own is not installed.
int pairsHere() {
    const int channels = bambi::devices::loopChannels();
    return std::clamp((channels > 0 ? channels : 16) / 2 - 1, 1, kPairs);
}

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
    Content() : pairs_(pairsHere()), device_({pairs_, bambi::loop::installed(), 0.0, 48000.0}) {
        //  The port first: the device starts the stand-in host when nothing answers there, and loading the
        //  plugins takes seconds.
        device_.transport = [this](const bambi::live::TransportMessage& m, std::int64_t stamp) {
            engine_.transport(m, stamp);
        };
        device_.slot = [this](const bambi::live::SlotPlay& p, std::int64_t stamp) {
            engine_.slot(p, stamp);
            timeline_.slotPlayed(p);
        };
        device_.changed = [this] { update(); };
        device_.restored = [this] {
            if (const bambi::Json* states = device_.session().kept("plugins")) {
                engine_.restore(*states);
                kept_ = states->dump(0);
            }
            if (const bambi::Json* lanes = device_.session().kept("lanes")) {
                lanes_ = bambi::live::lanesFrom(*lanes);
                keptLanes_ = lanes->dump(0);
                engine_.setLanes(lanes_);
            }
            //  the order the set was played at, before its plugins are laid out again
            if (const bambi::Json* order = device_.session().kept("order"); order != nullptr && order->isNumber()) {
                const int n = std::clamp(static_cast<int>(std::lround(order->numberOr(3.0))), 1, 7);
                keptOrder_ = n;
                if (n != engine_.order()) applyOrder(n);
            }
        };
        device_.show = [this] {
            if (auto* top = getTopLevelComponent()) top->toFront(true);
            juce::Process::makeForegroundProcess();
        };
        listening_ = device_.start(&error_);
        running_ = engine_.start({3, pairs_, 64, 48000.0, 0, bambi::livehost::loadAudioSettings()}, started_);
        device_.setLatency(engine_.latencyMs(), 48000.0);

        addAndMakeVisible(rail_);
        addChildComponent(routing_);
        addChildComponent(timeline_);
        timeline_.command = [this](const bambi::live::Command& c) { device_.command(c); };
        timeline_.heard = [this] { return engine_.heard(); };
        timeline_.refresh = [this] { device_.refresh(); };
        timeline_.lanesChanged = [this](std::vector<bambi::live::Lane> lanes) {
            lanes_ = std::move(lanes);
            engine_.setLanes(lanes_);
        };
        timeline_.plays = [this](const bambi::live::Lane& l) {
            return engine_.targetOf(l.instance, l.owner, l.param).plugin != nullptr;
        };
        routing_.reshape = [this](bambi::live::Shape shape) { device_.reshape(std::move(shape)); };
        //  a wire's level while it is dragged: heard at once, the plan as it is, kept only on release
        routing_.preview = [this](const bambi::live::Shape& shape) {
            const auto& session = device_.session();
            engine_.setSends(bambi::live::layOut(session.tree(), shape), session.tree());
        };
        routing_.showDecoder = [this](std::uint64_t id) { showView(View::Decoder, engine_.decoderOf(id)); };
        routing_.showSource = [this](bambi::live::TrackId track) { showView(View::Encoder, engine_.encoderOf(track)); };
        routing_.showInsert = [this](std::uint64_t insert, bambi::live::Effect effect) {
            showView(effect == bambi::live::Effect::Reverb ? View::Reverb : View::Echo, engine_.effectOf(insert));
        };
        addChildComponent(sheet_);
        sheet_.change = [this](bambi::live::AudioSettings settings) { applyAudio(std::move(settings)); };
        sheet_.changeOrder = [this](int order) { applyOrder(order); };
        sheet_.changeInputs = [this](std::vector<bambi::live::HostInput> inputs,
                                     bambi::live::AudioSettings settings) {
            applyAudio(std::move(settings));
            auto shape = device_.session().shape();
            shape.inputs = std::move(inputs);
            rebuild_ = true;  // a binding moved alone changes no field: build the program again regardless
            device_.reshape(std::move(shape));
            update();
        };
        sheet_.measure = [this](bool on) {
            measuring_ = on;
            lead_ = {};
            engine_.setMeasuring(on);
            sheet_.setLead(0.0, 0.0, 0);
        };
        sheet_.openDecoder = [this](std::uint64_t id) {
            sheet_.close();
            showView(View::Decoder, engine_.decoderOf(id));
        };
        sheet_.closed = [this] {
            if (editor_ != nullptr) editor_->setVisible(true);
            update();
        };
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
        routing_.setBounds(area);
        timeline_.setBounds(area);
        sheet_.setBounds(getLocalBounds());
    }

private:
    enum class View { Encoder, Echo, Reverb, Decoder, Routing, Timeline, Audio, Gap };  ///< Gap: between the rail's groups

    //  The plugins' states, kept with the set when they changed. Only while bambi is on: a device sends what the set
    //  kept before it switches on, and the defaults must not be kept over it.
    void timerCallback() override {
        sheet_.setLoad(engine_.load());
        //  Live's lead, as it is measured; stopped with the sheet
        if (measuring_ && !sheet_.isOpen()) {
            measuring_ = false;
            engine_.setMeasuring(false);
        }
        bool measured = false;
        while (const auto ms = engine_.measured()) {
            lead_.add(*ms);
            measured = true;
        }
        if (measured) sheet_.setLead(lead_.median(), lead_.spread(), lead_.count());
        //  what a Decoder's own page changed -- an output lit, moved, a layout -- reaches the routes
        if (engine_.reroute() && sheet_.isOpen()) showSheet();
        //  a device plugged in or out while the sheet is up: shown as it comes
        if (sheet_.isOpen())
            if (auto system = bambi::devices::system(); system != shownSystem_) showSheet();
        engine_.tidy();
        //  Live gone: the host quits after a while; the device starts it again when the set is opened
        if (device_.session().connected()) {
            heard_ = true;
            alone_ = 0;
        } else if (heard_ && ++alone_ >= kQuitAfterSeconds)
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        if (!device_.session().on()) return;
        //  the order with the set, and the lanes, as the plugins' states are
        if (engine_.order() != keptOrder_) {
            keptOrder_ = engine_.order();
            device_.keep("order", bambi::Json(keptOrder_));
        }
        if (bambi::Json lanes = bambi::live::toJson(lanes_); lanes.dump(0) != keptLanes_) {
            keptLanes_ = lanes.dump(0);
            device_.keep("lanes", std::move(lanes));
        }
        bambi::Json states = engine_.states();
        std::string text = states.dump(0);
        if (text == kept_) return;
        kept_ = std::move(text);
        device_.keep("plugins", std::move(states));
    }

    void update() {
        const auto& session = device_.session();
        if (rebuild_ || session.pairs() != onPairs_ || !(session.field() == onField_)) {
            rebuild_ = false;
            onPairs_ = session.pairs();
            onField_ = session.field();
            juce::String status;
            const bool changed = engine_.setField(session.field(), session.pairs(), session.shape().inputs, status);
            //  only once Live has said what its set holds: before, an empty tree would forget every kept state
            if (session.hasTree()) engine_.forget(session.tree(), session.shape());
            problem_ = changed || status.isEmpty() ? juce::String() : status;
            if (changed) {
                named_.fill({});
                device_.setLatency(engine_.latencyMs(), 48000.0);
                //  a window on an Encoder let go of closes before that Encoder can be freed
                if (view_ != View::Routing && view_ != View::Timeline && !engine_.holds(shownPlugin_))
                    showView(view_);
            }
        }
        engine_.setSends(session.field(), session.tree());
        for (int pair = 0; pair < pairs_; ++pair) {
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
        //  the routing page after a gap: the column's labels and its views stay one to one
        labels.push_back({});
        views_.push_back(View::Gap);
        labels.push_back("routing");
        views_.push_back(View::Routing);
        if (view_ == View::Routing) selected = static_cast<int>(views_.size()) - 1;
        labels.push_back("timeline");
        views_.push_back(View::Timeline);
        if (view_ == View::Timeline) selected = static_cast<int>(views_.size()) - 1;
        //  a clip lane after its clip, wherever the scenes have moved it
        if (bambi::live::followClips(lanes_, session.arrangement())) engine_.setLanes(lanes_);
        timeline_.show({session.tree(), session.arrangement(), session.shape(), session.field(), lanes_});
        //  the audio sheet after another gap: over the window, never a view of its own
        labels.push_back({});
        views_.push_back(View::Gap);
        labels.push_back("audio");
        views_.push_back(View::Audio);
        if (sheet_.isOpen()) selected = static_cast<int>(views_.size()) - 1;
        routing_.show(session.tree(), session.shape(), session.field(), session.pairs());
        routing_.showDecoders(engine_.playing());
        rail_.setViews(labels, selected, [this](int i) {
            const View v = views_[static_cast<std::size_t>(i)];
            if (v == View::Gap) return;
            if (v != View::Audio) {
                showView(v);
                return;
            }
            showSheet();
            //  a plugin's own window is the system's, drawn over anything of the host's: hidden while the sheet is up
            if (editor_ != nullptr) editor_->setVisible(false);
            sheet_.open();
            update();
        });
        rail_.setStatus(running_ ? describe(device_.session(), listening_, error_, problem_, engine_.latencyMs())
                                 : bambi::livehost::Rail::Status{false, {"audio", "off"}, started_});
    }

    //  The sheet's change: kept on this computer, then played; the sheet shown again as the engine has it.
    void applyAudio(bambi::live::AudioSettings settings) {
        if (!bambi::livehost::saveAudioSettings(settings)) audioProblem_ = "the settings could not be saved";
        juce::String status;
        audioProblem_ = engine_.setAudio(settings, status) ? juce::String() : status;
        device_.setLatency(engine_.latencyMs(), engine_.rate());
        showSheet();
        update();
    }

    //  Every plugin at another order: loaded again, their states kept; the field built again on them.
    void applyOrder(int order) {
        juce::String status;
        audioProblem_ = engine_.setOrder(order, status) ? juce::String() : status;
        rebuild_ = true;
        device_.setLatency(engine_.latencyMs(), engine_.rate());
        update();
        if (sheet_.isOpen()) showSheet();
    }

    void showSheet() {
        bambi::livehost::AudioSheet::Shown shown;
        shown.settings = engine_.audio();
        shown.system = bambi::devices::system();
        shownSystem_ = shown.system;
        shown.decoders = engine_.playing();
        shown.inputs = device_.session().shape().inputs;
        shown.rate = engine_.rate();
        shown.order = engine_.order();
        shown.load = engine_.load();
        shown.midiInputs = engine_.midiInputs();
        shown.measuring = measuring_;
        shown.leadMs = lead_.median();
        shown.leadSpreadMs = lead_.spread();
        shown.leadCount = lead_.count();
        shown.problem = audioProblem_;
        sheet_.show(std::move(shown));
    }

    juce::AudioProcessor* pluginOf(View v) const {
        switch (v) {
            case View::Encoder: return engine_.firstEncoder();
            case View::Echo: return engine_.firstEffect(bambi::live::Effect::Echo);
            case View::Reverb: return engine_.firstEffect(bambi::live::Effect::Reverb);
            case View::Decoder: return engine_.decoder();
            case View::Routing:
            case View::Timeline:
            case View::Audio:
            case View::Gap: return nullptr;
        }
        return nullptr;
    }

    static const char* nameOf(View v) {
        switch (v) {
            case View::Encoder: return "encoder";
            case View::Echo: return "echo";
            case View::Reverb: return "reverb";
            case View::Decoder: return "decoder";
            case View::Routing: return "routing";
            case View::Timeline: return "timeline";
            case View::Audio: return "audio";
            case View::Gap: return "";
        }
        return "";
    }

    //  `v`, showing `which` of its plugins when given, else the first
    void showView(View v, juce::AudioProcessor* which = nullptr) {
        if (editor_ != nullptr) removeChildComponent(editor_.get());
        editor_.reset();
        if (v != View::Routing && v != View::Timeline && which == nullptr && pluginOf(v) == nullptr) v = View::Decoder;
        auto* plugin = which != nullptr ? which : pluginOf(v);
        routing_.setVisible(v == View::Routing);
        timeline_.setVisible(v == View::Timeline);
        view_ = v;
        shownPlugin_ = plugin;
        if (plugin != nullptr) editor_.reset(plugin->createEditorAndMakeActive());
        if (editor_ != nullptr) {
            addChildComponent(*editor_);
            editor_->setVisible(!sheet_.isOpen());
        }
        const int w = editor_ != nullptr ? editor_->getWidth() : bambi::ui::theme::metrics::windowWidth;
        const int h = editor_ != nullptr ? editor_->getHeight() : bambi::ui::theme::metrics::windowHeight;
        setSize(bambi::ui::theme::rail::width + w, h);
        resized();
        update();
    }

    int pairs_;  ///< the loop device's track pairs here: before everything sized by them
    bambi::livehost::Engine engine_;
    bool running_{false};
    juce::String started_;  ///< what the engine said when it started, or why it did not
    juce::String problem_;  ///< why the last change to what it plays could not be made
    bambi::livehost::Rail rail_;
    bambi::livehost::RoutingPage routing_;
    bambi::livehost::TimelinePage timeline_;
    bambi::livehost::AudioSheet sheet_;
    juce::String audioProblem_;  ///< why the sheet's last change could not be played or kept
    bool measuring_{false};      ///< Live's lead is measured, while the sheet is open
    int keptOrder_{0};           ///< the order last kept with the set
    bambi::live::MidiLead lead_;  ///< what has been measured since measuring began
    std::vector<bambi::live::Lane> lanes_;  ///< the host's automation, kept with the set
    std::string keptLanes_;                 ///< the lanes as last kept
    bool rebuild_{false};  ///< the program to be built again at the next update, whatever changed
    std::vector<bambi::live::AudioDevice> shownSystem_;  ///< the system's devices, as the sheet last showed them
    std::vector<View> views_;
    View view_{View::Decoder};
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    bool listening_{false};
    std::string error_;                            ///< why the port could not be opened
    std::vector<bambi::live::TrackPair> onPairs_;  ///< the pairs and the field the program was last built for
    bambi::live::Field onField_;
    juce::AudioProcessor* shownPlugin_{nullptr};  ///< the instance the window shows
    std::string kept_;                            ///< the plugins' states as last kept or restored
    static constexpr int kQuitAfterSeconds = 60;  ///< alone, after a device has been here
    bool heard_{false};                           ///< a device has been connected
    int alone_{0};                                ///< seconds without one since
    std::array<juce::String, kPairs> named_{};    ///< each Encoder's track name and colour as last given
    bambi::livehost::DeviceServer device_;        ///< last: gone first, before what its events touch
};

class Window final : public juce::DocumentWindow {
public:
    Window()
        : DocumentWindow("bambi live host", bambi::ui::theme::colour::ground,
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
