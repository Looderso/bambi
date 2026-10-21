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
#include <cmath>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <string>

#include "IoPage.h"
#include "Device.h"
#include "Engine.h"
#include "HostLog.h"
#include "LiveDevice.h"
#include "Rail.h"
#include "Routing.h"
#include "Settings.h"
#include "Timeline.h"
#include "bambi/live/midi.hpp"
#include "bambi/ui/ProductName.h"
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
                    ". Is another host running?"};
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

class Content final : public juce::Component, public juce::KeyListener, private juce::Timer {
public:
    Content() : pairs_(pairsHere()), device_({pairs_, bambi::loop::installed(), 0.0, 48000.0}) {
        //  The port first: a device that finds nothing answering there starts another host, and loading the
        //  plugins takes seconds.
        device_.transport = [this](const bambi::live::TransportMessage& m, std::int64_t stamp) {
            engine_.transport(m, stamp);
            rail_.setPlaying(m.playing);
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
            if (auto* top = getTopLevelComponent()) {
                top->setVisible(true);  // closed is hidden: "open host" brings it back
                top->toFront(true);
            }
            juce::Process::makeForegroundProcess();
        };
        listening_ = device_.start(&error_);
        running_ = engine_.start({3, pairs_, 64, 48000.0, 0, bambi::livehost::loadAudioSettings()}, started_);
        device_.setLatency(engine_.latencyMs(), 48000.0);

        addAndMakeVisible(rail_);
        rail_.transport = [this](bool play) { playOrStop(play); };
        rail_.toStart = [this] { device_.command({bambi::live::Command::Action::Locate, 0.0}); };
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
        addChildComponent(io_);
        io_.change = [this](bambi::live::AudioSettings settings) { applyAudio(std::move(settings)); };
        io_.changeOrder = [this](int order) { applyOrder(order); };
        io_.changeInputs = [this](std::vector<bambi::live::HostInput> inputs, bambi::live::AudioSettings settings) {
            applyAudio(std::move(settings));
            auto shape = device_.session().shape();
            shape.inputs = std::move(inputs);
            rebuild_ = true;  // a binding moved alone changes no field: build the program again regardless
            device_.reshape(std::move(shape));
            update();
        };
        io_.measure = [this](bool on) {
            measuring_ = on;
            lead_ = {};
            engine_.setMeasuring(on);
            io_.setLead(0.0, 0.0, 0);
        };
        io_.openDecoder = [this](std::uint64_t id) { showView(View::Decoder, engine_.decoderOf(id)); };
        update();
        showView(View::Decoder);
        startTimer(1000);
    }

    ~Content() override { editor_.reset(); }  // the window before the instance it shows

    void paint(juce::Graphics& g) override { g.fillAll(bambi::ui::theme::colour::ground); }

    void resized() override {
        auto area = getLocalBounds();
        rail_.setBounds(area.removeFromLeft(bambi::livehost::Rail::width()));
        if (editor_ != nullptr) editor_->setBounds(area.withSize(editor_->getWidth(), editor_->getHeight()));
        routing_.setBounds(area);
        timeline_.setBounds(area);
        io_.setBounds(area);
    }

    /*  Space plays or stops Live from anywhere in the window: the window hears every key no component under it took,
        so a text field keeps its spaces. */
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override { return rail_.takeKey(key); }

private:
    //  Live asked to play on from where it is, or to stop.
    void playOrStop(bool play) {
        device_.command({play ? bambi::live::Command::Action::Continue : bambi::live::Command::Action::Stop, 0.0});
    }

    enum class View {
        Encoder,
        Echo,
        Reverb,
        Decoder,
        Routing,
        Timeline,
        Io,
        Gap
    };  ///< Gap: between the rail's groups

    //  The plugins' states, kept with the set when they changed. Only while bambi is on: a device sends what the set
    //  kept before it switches on, and the defaults must not be kept over it.
    void timerCallback() override {
        io_.setLoad(engine_.load());
        //  every locate the transport decided while Live played, so a jump heard can be told from a real locate
        if (const auto locates = engine_.locates(); locates != locatesLogged_) {
            if (locates > locatesLogged_) {
                const double moved = engine_.lastLocateSeconds();
                bambi::livehost::hostLog("a locate: Live " + std::to_string(std::abs(moved)).substr(0, 5) + " s " +
                                         (moved < 0.0 ? "behind" : "ahead of") + " where the host had it" +
                                         (locates - locatesLogged_ > 1
                                              ? " (" + std::to_string(locates - locatesLogged_) + " this second)"
                                              : ""));
            }
            locatesLogged_ = locates;
        }
        //  Live's lead, as it is measured; stopped when the page is left
        if (measuring_ && view_ != View::Io) {
            measuring_ = false;
            engine_.setMeasuring(false);
        }
        bool measured = false;
        while (const auto ms = engine_.measured()) {
            lead_.add(*ms);
            measured = true;
        }
        if (measured) io_.setLead(lead_.median(), lead_.spread(), lead_.count());
        //  a click on a Decoder's map: the output placed where it asked, as the I/O page's map would
        for (const bambi::live::Placed& asked : engine_.placementsAsked()) {
            auto settings = engine_.audio();
            const auto had = std::find_if(settings.placed.begin(), settings.placed.end(),
                                          [&](const bambi::live::Placed& p) { return p.out == asked.out; });
            if (had != settings.placed.end())
                had->at = asked.at;
            else
                settings.placed.push_back(asked);
            applyAudio(std::move(settings), false);
        }
        //  what a Decoder's own page changed -- an output lit, moved, a layout -- reaches the routes
        if (engine_.reroute() && view_ == View::Io) showIo();
        //  a device plugged in or out while the page is open: shown as it comes
        if (view_ == View::Io)
            if (auto system = bambi::devices::system(); system != shownSystem_) showIo();
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
                if (view_ != View::Routing && view_ != View::Timeline && view_ != View::Io && !engine_.holds(shownPlugin_))
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
        //  the host's own pages after a gap, at the column's foot: I/O, routing, the timeline. The column's labels and
        //  its views stay one to one.
        labels.push_back({});
        views_.push_back(View::Gap);
        for (const View page : {View::Io, View::Routing, View::Timeline}) {
            labels.push_back(nameOf(page));
            views_.push_back(page);
            if (view_ == page) selected = static_cast<int>(views_.size()) - 1;
        }
        //  a clip lane after its clip, wherever the scenes have moved it
        if (bambi::live::followClips(lanes_, session.arrangement())) engine_.setLanes(lanes_);
        timeline_.show({session.tree(), session.arrangement(), session.shape(), session.field(), lanes_});
        routing_.show(session.tree(), session.shape(), session.field(), session.pairs());
        routing_.showDecoders(engine_.playing());
        rail_.setViews(labels, selected, [this](int i) {
            const View v = views_[static_cast<std::size_t>(i)];
            if (v != View::Gap) showView(v);
        });
        rail_.setStatus(running_ ? describe(device_.session(), listening_, error_, problem_, engine_.latencyMs())
                                 : bambi::livehost::Rail::Status{false, {"audio", "off"}, started_});
    }

    //  The I/O page's change: kept on this computer, then played; the page shown again as the engine has it.
    void applyAudio(bambi::live::AudioSettings settings, bool show = true) {
        if (!bambi::livehost::saveAudioSettings(settings)) audioProblem_ = "the settings could not be saved";
        juce::String status;
        audioProblem_ = engine_.setAudio(settings, status) ? juce::String() : status;
        device_.setLatency(engine_.latencyMs(), engine_.rate());
        if (show || view_ == View::Io) showIo();
        update();
    }

    //  Every plugin at another order: loaded again, their states kept; the field built again on them.
    void applyOrder(int order) {
        juce::String status;
        audioProblem_ = engine_.setOrder(order, status) ? juce::String() : status;
        rebuild_ = true;
        device_.setLatency(engine_.latencyMs(), engine_.rate());
        update();
        if (view_ == View::Io) showIo();
    }

    void showIo() {
        bambi::livehost::IoPage::Shown shown;
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
        io_.show(std::move(shown));
    }

    juce::AudioProcessor* pluginOf(View v) const {
        switch (v) {
            case View::Encoder: return engine_.firstEncoder();
            case View::Echo: return engine_.firstEffect(bambi::live::Effect::Echo);
            case View::Reverb: return engine_.firstEffect(bambi::live::Effect::Reverb);
            case View::Decoder: return engine_.decoder();
            case View::Routing:
            case View::Timeline:
            case View::Io:
            case View::Gap: return nullptr;
        }
        return nullptr;
    }

    static juce::String nameOf(View v) {
        switch (v) {
            case View::Encoder: return bambi::ui::productName(bambi::Product::Encoder);
            case View::Echo: return bambi::ui::productName(bambi::Product::Echo);
            case View::Reverb: return bambi::ui::productName(bambi::Product::Reverb);
            case View::Decoder: return bambi::ui::productName(bambi::Product::Decoder);
            case View::Routing: return "routing";
            case View::Timeline: return "timeline";
            case View::Io: return "I/O";
            case View::Gap: return "";
        }
        return "";
    }

    //  `v`, showing `which` of its plugins when given, else the first
    void showView(View v, juce::AudioProcessor* which = nullptr) {
        if (editor_ != nullptr) removeChildComponent(editor_.get());
        editor_.reset();
        const bool page = v == View::Routing || v == View::Timeline || v == View::Io;
        if (!page && which == nullptr && pluginOf(v) == nullptr) v = View::Decoder;
        auto* plugin = which != nullptr ? which : pluginOf(v);
        routing_.setVisible(v == View::Routing);
        timeline_.setVisible(v == View::Timeline);
        io_.setVisible(v == View::Io);
        if (v == View::Io) showIo();
        view_ = v;
        shownPlugin_ = plugin;
        if (plugin != nullptr) editor_.reset(plugin->createEditorAndMakeActive());
        if (editor_ != nullptr) {
            addAndMakeVisible(*editor_);
        }
        const int w = editor_ != nullptr ? editor_->getWidth() : bambi::ui::theme::metrics::windowWidth;
        const int h = editor_ != nullptr ? editor_->getHeight() : bambi::ui::theme::metrics::windowHeight;
        setSize(bambi::livehost::Rail::width() + w, h);
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
    bambi::livehost::IoPage io_;
    juce::String audioProblem_;             ///< why the I/O page's last change could not be played or kept
    bool measuring_{false};                 ///< Live's lead is measured, while the I/O page is open
    std::uint32_t locatesLogged_{0};        ///< the engine's locate count, as last logged
    int keptOrder_{0};                      ///< the order last kept with the set
    bambi::live::MidiLead lead_;            ///< what has been measured since measuring began
    std::vector<bambi::live::Lane> lanes_;  ///< the host's automation, kept with the set
    std::string keptLanes_;                 ///< the lanes as last kept
    bool rebuild_{false};                   ///< the program to be built again at the next update, whatever changed
    std::vector<bambi::live::AudioDevice> shownSystem_;  ///< the system's devices, as the I/O page last showed them
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
        auto* content = new Content();
        setContentOwned(content, true);
        keys_ = content;
        addKeyListener(keys_);  // space, from wherever the keyboard is
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    /*  Closing the window hides it, as a Mac app's window does: the host keeps playing, its plugins and their link stay as they
        are, and "open host" on the device or a second launch brings it back. Quitting is the menu's (Cmd-Q); the host also quits
        by itself a minute after Live's device has gone. */
    ~Window() override { removeKeyListener(keys_); }  // before the content it points at goes

    void closeButtonPressed() override {
        setVisible(false);
        juce::Process::
            hide();  // the app too, so the next click on its icon brings it to the front, and the window with it
    }
    void reveal() {
        setVisible(true);
        toFront(true);
    }

private:
    /*  The Dock's click on a running app makes it active and nothing more -- JUCE does not take the Mac's "reopen" -- so the
        window is shown when the host becomes the frontmost app while hidden: a click on its icon, Cmd-Tab, "open host". */
    struct Reopen final : juce::Timer {
        explicit Reopen(Window& w) : window(w) { startTimerHz(4); }
        void timerCallback() override {
            const bool front = juce::Process::isForegroundProcess();
            if (front && !wasFront && !window.isVisible()) window.reveal();
            wasFront = front;
        }
        Window& window;
        bool wasFront{true};
    } reopen_{*this};

    juce::KeyListener* keys_{nullptr};  ///< the content, which owns what space reaches
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "bambi live host"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        bambi::livehost::installLiveDevice();
        window_ = std::make_unique<Window>();
    }
    //  opened again while running -- the device starting it, the Finder, the Dock -- shows the hidden window
    void anotherInstanceStarted(const juce::String&) override {
        if (window_ != nullptr) window_->reveal();
    }
    void shutdown() override { window_.reset(); }

private:
    std::unique_ptr<Window> window_;
};

}  // namespace

START_JUCE_APPLICATION(App)
