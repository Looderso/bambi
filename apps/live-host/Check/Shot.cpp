// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-live-host-shot: a PNG of the live host's window, with no Live and no audio device -- the host's own column
//  beside one of its pages, over a set built here: a drum group split into its tracks, a bass, keys left in Live, a
//  vocal through an Echo, a return through a Reverb, a microphone of the host's own, two Decoders.
//
//    bambi-live-host-shot out.png [mode] [--scale N]
//
//    (none), --routing   the routing page
//    --wires             the routing page, the vocal chosen: its wires open under its line
//    --timeline          the timeline, Live's arrangement and the host's lanes over it
//    --session           the timeline's session view
//    --interface, --outputs, --inputs, --midi
//                        the audio sheet over the routing page, on that tab
//
//  The plugins' views are not drawn: the window shows each installed plugin's own native view there, which nothing
//  headless can draw. Their own picture tools draw them.
//
//  Nothing here reads a clock, a device or the user's settings: drawn twice, the picture is the same.

#include <algorithm>
#include <cstdio>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <vector>

#include "../Source/AudioSheet.h"
#include "../Source/Rail.h"
#include "../Source/Routing.h"
#include "../Source/Timeline.h"
#include "bambi/host/ShotScale.h"
#include "bambi/ui/Theme.h"

using namespace bambi;

namespace {

enum Id : live::TrackId { kDrums = 1, kKick, kSnare, kHats, kBass, kKeys, kVox, kPlate = 10, kSlap };
constexpr live::TrackId kRoomMic = live::kFirstInput;
constexpr std::uint64_t kVoxEcho = 1, kPlateReverb = 2;
constexpr std::uint64_t kHeadphones = live::kFirstDecoder, kSpeakers = 2;

live::Node node(live::TrackId id, live::Node::Kind kind, std::optional<live::TrackId> parent, const char* name,
                const char* colour) {
    live::Node n;
    n.id = id;
    n.kind = kind;
    n.parent = parent;
    n.name = name;
    n.colour = colour;
    n.fader = 0.0;
    return n;
}

//  The set as Live would report it, and what the host has made of it.
struct Fixture {
    live::Tree tree;
    live::Shape shape;
    live::Field field;
    std::vector<live::TrackPair> pairs;
    live::Arrangement arrangement;
    std::vector<live::Lane> lanes;
    std::vector<live::PlayingDecoder> decoders;
};

Fixture fixture() {
    using Kind = live::Node::Kind;
    Fixture f;
    f.tree.nodes = {node(kDrums, Kind::Group, std::nullopt, "Drums", "#e0b552"),
                    node(kKick, Kind::Track, kDrums, "Kick", "#e0b552"),
                    node(kSnare, Kind::Track, kDrums, "Snare", "#e0b552"),
                    node(kHats, Kind::Track, kDrums, "Hats", "#e0b552"),
                    node(kBass, Kind::Track, std::nullopt, "Bass", "#c05a4a"),
                    node(kKeys, Kind::Track, std::nullopt, "Keys", "#6a9fd4"),
                    node(kVox, Kind::Track, std::nullopt, "Vox", "#7fb069"),
                    node(kPlate, Kind::Return, std::nullopt, "A Plate", "#9a8fc4"),
                    node(kSlap, Kind::Return, std::nullopt, "B Slap", "#9a8fc4")};

    f.shape.taken = {kDrums, kBass, kVox};
    f.shape.split = {kDrums};
    f.shape.returns = {kPlate};
    f.shape.decoders = {kHeadphones, kSpeakers};
    f.shape.chains = {{kVox, {{kVoxEcho, live::Effect::Echo, 0}}}, {kPlate, {{kPlateReverb, live::Effect::Reverb, 0}}}};
    f.shape.inputs = {{kRoomMic, "Room mic", 1}};
    f.shape.routing = {{kVox, {{live::Target::main(), 0.0}, {live::Target::node(kPlate), -8.0}}},
                       {kSnare, {{live::Target::node(kDrums), 0.0}, {live::Target::node(kPlate), -14.0}}}};
    f.field = live::layOut(f.tree, f.shape);
    f.pairs = {{kKick, 0}, {kSnare, 1}, {kHats, 2}, {kBass, 3}, {kVox, 4}};

    //  in quarter notes, four to a bar: forty-eight bars, in three sections
    auto& a = f.arrangement;
    a.tracks = {{kKick, {{0.0, 64.0, "Kick", "#e0b552", false}, {96.0, 192.0, "Kick chorus", "#e0b552", false}}},
                {kSnare, {{32.0, 64.0, "Snare", "#e0b552", false}, {96.0, 192.0, "Snare chorus", "#e0b552", false}}},
                {kHats, {{16.0, 192.0, "Hats", "#e0b552", false}}},
                {kBass, {{32.0, 96.0, "Bass verse", "#c05a4a", true}, {96.0, 192.0, "Bass chorus", "#c05a4a", true}}},
                {kKeys, {{0.0, 96.0, "Pad", "#6a9fd4", true}}},
                {kVox, {{32.0, 88.0, "Verse", "#7fb069", false}, {96.0, 184.0, "Chorus", "#7fb069", false}}}};
    a.locators = {{0.0, "intro"}, {32.0, "verse"}, {96.0, "chorus"}};
    a.loopOn = true;
    a.loopStart = 96.0;
    a.loopLength = 32.0;
    a.readMs = 4.0;
    a.scenes = 4;
    a.session = {{kKick, {{0, "Kick A", "#e0b552", 0.0, 4.0, true, false}, {2, "Fill", "#e0b552", 0.0, 8.0, true, false}}},
                 {kHats, {{0, "Hats", "#e0b552", 0.0, 4.0, true, false}, {1, "Hats open", "#e0b552", 0.0, 8.0, true, false}}},
                 {kBass, {{1, "Bass B", "#c05a4a", 0.0, 16.0, true, true}, {2, "Bass C", "#c05a4a", 0.0, 16.0, true, true}}},
                 {kVox, {{3, "Ad libs", "#7fb069", 0.0, 32.0, false, false}}}};

    f.lanes = {{1, live::Instance::Encoder, kVox, "transform.yaw", {{32.0, -90.0}, {64.0, 90.0}, {88.0, 0.0}}, 0, -1, {}},
               {2, live::Instance::Insert, kVoxEcho, "output.wet", {{32.0, 0.2}, {96.0, 0.2}, {128.0, 0.6}}, 0, -1, {}},
               {3, live::Instance::Decoder, kSpeakers, "output.gain", {{0.0, -12.0}, {32.0, 0.0}}, 0, -1, {}}};

    DecoderReport headphones, speakers;
    headphones.bus = 64;
    headphones.headphonesOn = true;
    headphones.headphoneChannel = 1;
    speakers.bus = 64;
    speakers.speakersOn = true;
    speakers.speakerFirstChannel = 3;
    speakers.speakerChannels = {3, 4, 5, 6, 7};
    speakers.layout = speakers.layoutName = "0+5+0";
    f.decoders = {{kHeadphones, headphones, 64}, {kSpeakers, speakers, 64}};
    return f;
}

//  Where the host plays and hears on a computer with one interface beside the loop device.
livehost::AudioSheet::Shown audio(const Fixture& f) {
    livehost::AudioSheet::Shown s;
    s.system = {{"interface", "Interface", 16, 8, 120, 100, false}, {"airpods", "AirPods", 2, 1, 7000, 7000, true}};
    s.settings.devices = {"interface"};
    s.settings.main = {"interface", 1};
    s.settings.placed = {{{kHeadphones, Output::Headphones}, {"interface", 3}},
                         {{kSpeakers, Output::Speakers}, {"interface", 5}}};
    s.settings.copies = {{{kHeadphones, Output::Headphones}, {}, true}};
    s.settings.inputs = {{kRoomMic, {"interface", 1}}};
    s.settings.liveMidiMs = 20.0;
    s.decoders = f.decoders;
    s.inputs = f.shape.inputs;
    s.order = 3;
    s.load = 0.24f;
    s.midiInputs = {"Keystation", "bambi"};
    return s;
}

//  The window as the host lays it out: its column on the left, the page beside it, the sheet over both.
class Window final : public juce::Component {
public:
    void paint(juce::Graphics& g) override { g.fillAll(ui::theme::colour::ground); }
    void resized() override {
        auto area = getLocalBounds();
        rail.setBounds(area.removeFromLeft(ui::theme::rail::width));
        routing.setBounds(area);
        timeline.setBounds(area);
        sheet.setBounds(getLocalBounds());
    }

    livehost::Rail rail;
    livehost::RoutingPage routing;
    livehost::TimelinePage timeline;
    livehost::AudioSheet sheet;
};

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String out = argc > 1 ? argv[1] : "/tmp/live-host.png";
    const float scale = host::takeShotScale(argc, argv);
    const juce::String mode = argc > 2 ? juce::String(argv[2]) : juce::String("--routing");

    std::optional<livehost::AudioSheet::Tab> tab;
    if (mode == "--interface") tab = livehost::AudioSheet::Tab::Interface;
    if (mode == "--outputs") tab = livehost::AudioSheet::Tab::Outputs;
    if (mode == "--inputs") tab = livehost::AudioSheet::Tab::Inputs;
    if (mode == "--midi") tab = livehost::AudioSheet::Tab::Midi;
    const bool timeline = mode == "--timeline" || mode == "--session";
    if (!timeline && !tab && mode != "--routing" && mode != "--wires") {
        std::fprintf(stderr, "unknown mode %s\n", mode.toRawUTF8());
        return 1;
    }

    const Fixture f = fixture();
    Window window;
    window.addAndMakeVisible(window.rail);
    window.addChildComponent(window.routing);
    window.addChildComponent(window.timeline);
    window.addChildComponent(window.sheet);
    window.setSize(ui::theme::rail::width + ui::theme::metrics::windowWidth, ui::theme::metrics::windowHeight);

    //  the column as the host fills it with every kind of plugin playing: the plugins, a gap, the pages, a gap,
    //  the sheet
    const std::vector<juce::String> labels{"encoder", "echo", "reverb", "decoder", {}, "routing", "timeline", {},
                                           "audio"};
    const juce::String open = tab ? "audio" : timeline ? "timeline" : "routing";
    const auto at = std::find(labels.begin(), labels.end(), open) - labels.begin();
    jassert(at < static_cast<std::ptrdiff_t>(labels.size()));
    window.rail.setViews(labels, static_cast<int>(at), [](int) {});
    const int sources = static_cast<int>(f.pairs.size());
    window.rail.setStatus({true, {"live", juce::String(sources) + " sources", "5.3 ms"}, {}});

    window.routing.show(f.tree, f.shape, f.field, f.pairs);
    window.routing.showDecoders(f.decoders);
    if (mode == "--wires") window.routing.choose(kVox);

    //  stopped, so the session view's clip playheads read no clock
    window.timeline.heard = [] { return live::Playhead{false, 0, 104.0, 118.0}; };
    window.timeline.plays = [](const live::Lane&) { return true; };
    window.timeline.show({f.tree, f.arrangement, f.shape, f.field, f.lanes});
    //  two session clips playing; only in the session view, since the arrangement would mark their tracks the session's
    if (mode == "--session") {
        window.timeline.slotPlayed({kKick, 0, 1.0, 0.0, 4.0, true, true});
        window.timeline.slotPlayed({kBass, 1, 6.0, 0.0, 16.0, true, true});
    }
    window.timeline.showSession(mode == "--session");

    window.routing.setVisible(!timeline);
    window.timeline.setVisible(timeline);
    if (tab) {
        window.sheet.show(audio(f));
        window.sheet.showTab(*tab);
        window.sheet.open();
    }

    const juce::Image shot = window.createComponentSnapshot(window.getLocalBounds(), true, scale);
    juce::File file(out);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    if (!png.writeImageToStream(shot, stream)) return 1;
    std::printf("%s  %d x %d\n", out.toRawUTF8(), shot.getWidth(), shot.getHeight());
    return 0;
}
