// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PluginProcessor.h"
#include "bambi/decode/layouts.hpp"
#include "bambi/editor/CheckKit.h"

/*  The decoder's host contract, headless: an ambisonic input at the order that fits, an output as wide as the host
 *  gives, every output a set of channels on it, silence where nothing plays; the state round trip; renders that
 *  do not depend on how the host cuts its blocks. Exit code 0 when every check passes.
 */
using namespace bambi;
using namespace bambi::editor;
using Buses = bambi::editor::Layout;  // a host's bus layout; bambi::Layout is a loudspeaker layout

namespace {
Buses busIn(int inChannels, int outChannels) {
    Buses l;
    l.inputBuses.add(inChannels == 4 || inChannels == 9 || inChannels == 16 || inChannels == 25 || inChannels == 36 ||
                             inChannels == 49 || inChannels == 64
                         ? amb(host::orderForChannels(inChannels))
                         : juce::AudioChannelSet::discreteChannels(inChannels));
    l.outputBuses.add(outChannels == 2 ? juce::AudioChannelSet::stereo()
                                       : juce::AudioChannelSet::discreteChannels(outChannels));
    return l;
}

/// The checks' own render: like renderField, but a layout with one input bus, no sidechain.
Channels render(const Buses& l, int block, double seconds, const std::function<void(BambiDecoderProcessor&)>& setUp,
                const PluginState* patch = nullptr, const std::function<void(BambiDecoderProcessor&)>& after = {}) {
    BambiDecoderProcessor proc;
    if (!proc.setBusesLayout(l)) return {};
    host::TestPlayHead head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(kCheckRate, block);
    if (setUp) setUp(proc);
    proc.prepareToPlay(kCheckRate, block);
    if (patch != nullptr) proc.commitDocument(*patch, true);
    const int inChannels = l.getMainInputChannelSet().size(), outChannels = l.getMainOutputChannelSet().size();
    const int width = std::max(inChannels, outChannels);
    const auto frames = static_cast<juce::int64>(seconds * kCheckRate);
    Channels captured(static_cast<std::size_t>(outChannels));
    juce::AudioBuffer<float> buffer(width, block);
    juce::MidiBuffer midi;
    for (juce::int64 at = 0; at < frames; at += block) {
        head.time = at;
        buffer.clear();
        for (int c = 0; c < inChannels; ++c) {
            float* x = buffer.getWritePointer(c);
            for (int i = 0; i < block; ++i) x[i] = fieldSample(c, at + i);
        }
        proc.processBlock(buffer, midi);
        for (int c = 0; c < outChannels; ++c) {
            const float* y = buffer.getReadPointer(c);
            captured[static_cast<std::size_t>(c)].insert(captured[static_cast<std::size_t>(c)].end(), y, y + block);
        }
    }
    if (after) after(proc);
    return captured;
}

double channelEnergy(const Channels& c, int channel) {
    double e = 0;
    for (float v : c[static_cast<std::size_t>(channel)]) e += static_cast<double>(v) * v;
    return e;
}

PluginState withLayout(const char* name, bool speakers, bool headphones, int hpChannel = 1) {
    PluginState s{decodeParams()};
    const bambi::Layout* l = bs2051Layout(name);
    s.decoder.layout = l->speakers;
    s.decoder.layoutName = name;
    s.decoder.speakersPlay = speakers;
    s.decoder.headphonesPlay = headphones;
    s.decoder.headphoneChannel = hpChannel;
    return s;
}
}  // namespace

/// A state loaded as a session is: the editor starts from it too, so an edit that follows builds on it.
void loadState(BambiDecoderProcessor& proc, const PluginState& s) {
    proc.document().adopt(s);
    proc.commitDocument(s, true);
}

/// A yaw alone as the known /ypr address carries it: yaw, pitch and roll in degrees.
std::vector<float> yprValues(float yawDeg) { return {yawDeg, 0.0f, 0.0f}; }

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("bambi Decoder: host contract and renders\n\n");

    //  Buses: the field in at any width that holds an order, the output two or more channels, never renegotiated.
    {
        BambiDecoderProcessor proc;
        for (int in : {4, 9, 16, 18, 64})
            for (int out : {2, 8, 18, 64})
                check(proc.checkBusesLayoutSupported(busIn(in, out)),
                      "accepts in " + juce::String(in) + " out " + juce::String(out));
        check(!proc.checkBusesLayoutSupported(busIn(16, 1)), "refuses a one-channel output");
        check(proc.modelLoaded(), "both heads are in the bundle: the model and the KU100");
        check(proc.getLatencySamples() == 0 || proc.getLatencySamples() == BambiDecoderProcessor::kPartition,
              "latency is the convolution's partition, or none before prepare");
    }

    //  Headphones by default: the pair on 1-2, the rest of the bus silent.
    {
        const Channels y = render(busIn(16, 8), 128, 0.5, {});
        check(!y.empty() && channelEnergy(y, 0) > 1e-3 && channelEnergy(y, 1) > 1e-3,
              "headphones play on channels 1 and 2");
        bool silent = true;
        for (int c = 2; c < 8; ++c) silent = silent && channelEnergy(y, c) == 0.0;
        check(silent, "channels 3-8 are silent: nothing plays there");
    }

    //  A layout on its channels: 7.1.4 fills 1-3 and 5-12, 4 is the LFE's and stays silent; headphones off.
    {
        const PluginState s = withLayout("4+7+0", true, false);
        std::array<float, 18> peaks{};
        const Channels y = render(busIn(16, 18), 128, 0.5, {}, &s, [&peaks](BambiDecoderProcessor& p) {
            for (std::size_t c = 0; c < peaks.size(); ++c) peaks[c] = p.diagnostics().channelPeak[c].load();
        });
        bool on = !y.empty();
        for (int c : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11}) on = on && channelEnergy(y, c) > 1e-4;
        check(on, "7.1.4 plays on channels 1-3 and 5-12");
        check(!y.empty() && channelEnergy(y, 3) == 0.0, "channel 4, the LFE, is silent");
        bool rest = !y.empty();
        for (int c = 12; c < 18; ++c) rest = rest && channelEnergy(y, c) == 0.0;
        check(rest, "channels 13-18 are silent");
        //  Catches: a per-channel peak stored one channel off, or never stored: the LFE's and the spare channels'
        //  meters read zero while the loudspeakers' read the last block's peak.
        bool metersAgree = peaks[0] > 1e-3f && peaks[2] > 1e-3f && peaks[11] > 1e-3f && peaks[3] == 0.0f;
        for (std::size_t c = 12; c < 18; ++c) metersAgree = metersAgree && peaks[c] == 0.0f;
        check(metersAgree, "the channel meters read the channels that play, and zero on the LFE and the spares");
        //  The same on a bus too narrow for it: it does not play, and says how many channels it needs.
        const Channels narrow = render(busIn(16, 8), 128, 0.25, {}, &s);
        check(!narrow.empty() && energy(narrow) == 0.0, "a layout needing 12 channels does not play on 8");
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 8));
        proc.commitDocument(s, true);
        proc.playOutput(BambiDecoderProcessor::Output::Speakers, true);
        check(proc.notice().contains("12 channels needed"), "and the notice says how many: " + proc.notice());
    }

    //  Two outputs at once when their channels do not collide: 7.1.4 on 1-12, headphones moved to 13-14.
    {
        const PluginState s = withLayout("4+7+0", true, true, 13);
        const Channels y = render(busIn(16, 18), 128, 0.5, {}, &s);
        check(!y.empty() && channelEnergy(y, 12) > 1e-3 && channelEnergy(y, 13) > 1e-3 && channelEnergy(y, 0) > 1e-4,
              "headphones on 13-14 play beside the layout on 1-12");
        //  Lighting headphones on 1-2 beside the layout moves them to 13-14 while the bus has room, and stops the
        //  layout when it has not. Catches: the move landing on an even pair, the room checked against the wrong
        //  width, the other output stopped although there was room.
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 18));
        loadState(proc, withLayout("4+7+0", true, false, 1));
        proc.playOutput(BambiDecoderProcessor::Output::Headphones, true);
        const DecoderState after = proc.decoderState();
        check(after.headphonesPlay && after.speakersPlay && after.headphoneChannel == 13 &&
                  proc.notice().contains("moved to 13"),
              "headphones lit on the layout's channels move past it and both play: " + proc.notice());
        BambiDecoderProcessor narrow;
        narrow.setBusesLayout(busIn(16, 12));
        loadState(narrow, withLayout("4+7+0", true, false, 1));
        narrow.playOutput(BambiDecoderProcessor::Output::Headphones, true);
        const DecoderState stopped = narrow.decoderState();
        check(stopped.headphonesPlay && !stopped.speakersPlay && narrow.notice().contains("speakers stopped"),
              "and on a bus with no room the layout stops and says so: " + narrow.notice());
        PluginState hpFirst = withLayout("4+7+0", false, true, 1);
        BambiDecoderProcessor movedLayout;
        movedLayout.setBusesLayout(busIn(16, 18));
        loadState(movedLayout, hpFirst);
        movedLayout.playOutput(BambiDecoderProcessor::Output::Speakers, true);
        const DecoderState shifted = movedLayout.decoderState();
        check(shifted.speakersPlay && shifted.headphonesPlay && shifted.speakerFirstChannel == 3,
              "a layout lit on the headphones' channels moves to start after them");
    }

    //  The stereo pair: a layout of two plays on channels 1-2 as virtual cardioids, so nothing in the field vanishes.
    {
        const PluginState s = withLayout("0+2+0", true, false);
        const Channels y = render(busIn(16, 4), 128, 0.25, {}, &s);
        check(!y.empty() && channelEnergy(y, 0) > 1e-4 && channelEnergy(y, 1) > 1e-4 && channelEnergy(y, 2) == 0.0,
              "a stereo pair plays on channels 1 and 2 and nowhere else");
        //  Catches: the head reaching the loudspeakers when the pair is a fold, or not reaching them when it is a
        //  slice; the microphones' settings not reaching the design; the scene's turn missing an output.
        const auto headTurned = [](BambiDecoderProcessor& p) { p.takeOrientation("/ypr", yprValues(180.0f)); };
        PluginState tracked = s;
        tracked.decoder.trackerSource = 1;
        const Channels still = render(busIn(16, 4), 128, 0.25, headTurned, &tracked);
        check(!still.empty() && identical(y, still), "the head turns nothing for a fold: the pair stays in the room");
        PluginState slice = tracked;
        slice.decoder.pairSlice = true;
        const Channels ahead = render(busIn(16, 4), 128, 0.25, {}, &slice),
                       turnedAway = render(busIn(16, 4), 128, 0.25, headTurned, &slice);
        check(!ahead.empty() && !identical(ahead, y), "the slice is another design than the fold");
        check(!turnedAway.empty() && !identical(ahead, turnedAway), "and the head turns what the slice plays");
        //  The scene's turn reaches every output: the fold hears yaw, and the tracker turning the scene is heard
        //  where the head was not. Static holds the sliders at zero.
        const auto sceneTurned = [](BambiDecoderProcessor& p) { setParameter(p, "head.yaw", 180.0f); };
        const Channels sceneOnFold = render(busIn(16, 4), 128, 0.25, sceneTurned, &s);
        check(!sceneOnFold.empty() && !identical(y, sceneOnFold), "yaw turns the scene the fold plays");
        PluginState trackerScene = tracked;
        trackerScene.decoder.trackerTurnsScene = true;
        const Channels byTracker = render(busIn(16, 4), 128, 0.25, headTurned, &trackerScene);
        check(!byTracker.empty() && !identical(y, byTracker), "and the tracker turns the scene when told to");
        PluginState held = s;
        held.decoder.sceneStatic = true;
        const Channels heldStill = render(busIn(16, 4), 128, 0.25, sceneTurned, &held);
        check(!heldStill.empty() && identical(y, heldStill), "static holds the scene whatever the sliders say");
        //  Catches: a rate not integrated, or integrated while the scene is static.
        const auto spinning = [](BambiDecoderProcessor& p) { setParameter(p, "head.yaw_rate", 90.0f); };
        const Channels spun = render(busIn(16, 4), 128, 0.25, spinning, &s),
                       spunHeld = render(busIn(16, 4), 128, 0.25, spinning, &held);
        check(!spun.empty() && !identical(y, spun) && identical(y, spunHeld),
              "a yaw rate turns the scene over time, and not while static");
    }

    //  The layout moved as a block: from channel 3, a pair plays on 3-4, and headphones on 1-2 play beside it.
    //  Catches: the shift not reaching the audio thread or the page; the bus edge checked unshifted.
    {
        PluginState s = withLayout("0+2+0", true, true);
        s.decoder.speakerFirstChannel = 3;
        const Channels y = render(busIn(16, 4), 128, 0.25, {}, &s);
        check(!y.empty() && channelEnergy(y, 0) > 1e-4 && channelEnergy(y, 1) > 1e-4 && channelEnergy(y, 2) > 1e-4 &&
                  channelEnergy(y, 3) > 1e-4,
              "headphones on 1-2 and a pair from channel 3 play together on four channels");
        s.decoder.headphonesPlay = false;
        const Channels pairOnly = render(busIn(16, 4), 128, 0.25, {}, &s);
        check(!pairOnly.empty() && channelEnergy(pairOnly, 0) == 0.0 && channelEnergy(pairOnly, 1) == 0.0 &&
                  channelEnergy(pairOnly, 2) > 1e-4,
              "the pair from channel 3 leaves 1-2 silent");
        s.decoder.speakerFirstChannel = 4;  // 4-5 do not fit a bus of 4
        const Channels over = render(busIn(16, 4), 128, 0.25, {}, &s);
        check(!over.empty() && channelEnergy(over, 3) == 0.0, "a layout past the bus edge plays nothing");
    }

    //  The test burst: noise on the asked channel alone while the array's feeds hold back, over when it is over.
    //  Catches: the burst on the wrong channel, the array still playing under it, the burst never ending.
    {
        const PluginState s = withLayout("4+7+0", true, false);
        const Channels y = render(busIn(16, 18), 128, 0.2, [](BambiDecoderProcessor& p) { p.testChannel(5); }, &s);
        bool alone = !y.empty() && channelEnergy(y, 4) > 1e-4;
        for (int c = 0; c < 12; ++c)
            if (c != 4) alone = alone && channelEnergy(y, c) == 0.0;
        check(alone, "the test burst plays on channel 5 and the array holds back");
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 18));
        proc.prepareToPlay(kCheckRate, 128);
        loadState(proc, s);
        proc.testChannel(5);
        juce::AudioBuffer<float> buffer(18, 128);
        juce::MidiBuffer midi;
        for (int b = 0; b * 128 < static_cast<int>(0.5 * kCheckRate); ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
        }
        check(proc.testing() == 0, "and it is over after half a second");
    }

    //  Determinism: the same render twice, and however the host cuts its blocks.
    {
        const PluginState s = withLayout("4+7+0", true, true, 13);
        const double seconds = 512.0 * 48 / kCheckRate;  // a whole number of every block size tried
        const Channels a = render(busIn(16, 18), 128, seconds, {}, &s), b = render(busIn(16, 18), 128, seconds, {}, &s);
        check(identical(a, b), "two renders are bit-identical");
        const Channels c = render(busIn(16, 18), 64, seconds, {}, &s), d = render(busIn(16, 18), 512, seconds, {}, &s);
        check(identical(a, c) && identical(a, d), "the block size does not change a sample: " + firstDifference(a, c));
    }

    //  The head turns the field for headphones: yaw 90 against 0 is a different render on the pair.
    {
        const Channels straight =
            render(busIn(16, 2), 128, 0.5, [](BambiDecoderProcessor& p) { setParameter(p, "head.yaw", 0.0f); });
        const Channels turned =
            render(busIn(16, 2), 128, 0.5, [](BambiDecoderProcessor& p) { setParameter(p, "head.yaw", 90.0f); });
        check(!straight.empty() && !turned.empty() && !identical(straight, turned),
              "yaw turns what the headphones hear");
    }

    //  The listener's sliders reach the filters, and the state goes round through the host.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        proc.editDecoder("head size", [](DecoderState& d) {
            d.headSize = 1.2f;
            d.earShift = -0.5f;
        });
        const auto [shift, size] = proc.builtSliders();
        check(std::abs(shift + 0.5) < 1e-6 && std::abs(size - 1.2) < 1e-6,
              "an edit of the sliders rebuilds the filters");
        juce::MemoryBlock saved;
        proc.getStateInformation(saved);
        BambiDecoderProcessor other;
        other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        const DecoderState back = other.decoderState();
        check(std::abs(back.headSize - 1.2f) < 1e-6f && std::abs(back.earShift + 0.5f) < 1e-6f,
              "the sliders survive a save and a load");
    }

    //  The KU100 is another head on the same chain: chosen, it is built, heard, and kept through the host.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        proc.editDecoder("ku100", [](DecoderState& d) { d.hrtf = 1; });
        check(proc.builtHead() == 1 && proc.headModel().learnedFrom.find("KU100") != std::string::npos,
              "choosing the ku100 rebuilds the filters from it: " + proc.headModel().learnedFrom);
        PluginState ku{decodeParams()};
        ku.decoder.hrtf = 1;
        //  Catches: a choice that never reaches the chain, or a second head that is the first under another name.
        const Channels model = render(busIn(16, 2), 128, 0.25, {}), ku100 = render(busIn(16, 2), 128, 0.25, {}, &ku);
        check(!model.empty() && !ku100.empty() && !identical(model, ku100), "the ku100 is heard, not the model");
        juce::MemoryBlock saved;
        proc.getStateInformation(saved);
        BambiDecoderProcessor other;
        other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        check(other.decoderState().hrtf == 1, "the choice survives a save and a load");
    }

    //  Learn mode: the known table serves without learning; a learned address with its form turns the head and an
    //  address not learned is then ignored; three split addresses gather into one triple.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        proc.editDecoder("osc", [](DecoderState& d) { d.trackerSource = 1; });
        const auto headAfter = [&proc](const char* address, std::vector<float> values) {
            proc.takeOrientation(address, values);
            juce::AudioBuffer<float> buffer(16, 128);
            buffer.clear();
            juce::MidiBuffer midi;
            proc.processBlock(buffer, midi);
            return std::pair<float, float>{proc.diagnostics().headYaw.load(), proc.diagnostics().headPitch.load()};
        };
        check(std::abs(headAfter("/ypr", {30.0f, 0.0f, 0.0f}).first - 30.0f) < 1e-3f,
              "the known table turns the head without learning");
        //  Catches: a learned form not applied, the quaternion's w read at the wrong end, the known table still read.
        proc.editDecoder("learn", [](DecoderState& d) {
            d.oscAddresses = {"/zig/quat", "", ""};
            d.oscForm = 2;  // w last
        });
        const float h = 22.5f * 3.14159265f / 180.0f;  // a yaw of 45 degrees about z
        const auto learned = headAfter("/zig/quat", {0.0f, 0.0f, std::sin(h), std::cos(h)});
        check(std::abs(learned.first - 45.0f) < 1e-2f, "a learned quaternion with w last turns the head by 45");
        check(std::abs(headAfter("/ypr", {30.0f, 0.0f, 0.0f}).first - 45.0f) < 1e-2f,
              "and a known address is ignored while one is learned");
        //  Catches: a split slot applied before the others arrived, slots mixed up, the first number not taken.
        proc.editDecoder("split", [](DecoderState& d) {
            d.oscAddresses = {"/y", "/p", "/r"};
            d.oscForm = 3;
        });
        check(std::abs(headAfter("/y", {20.0f}).first - 45.0f) < 1e-2f,
              "one of three split values alone changes nothing");
        headAfter("/p", {-10.0f, 99.0f});
        const auto triple = headAfter("/r", {0.0f});
        check(std::abs(triple.first - 20.0f) < 1e-2f && std::abs(triple.second + 10.0f) < 1e-2f,
              "yaw, pitch and roll gathered from three addresses turn the head");
        const auto seen = proc.seenAddresses();
        check(seen.size() == 5, "learn mode lists what arrived: " + juce::String(seen.size()) + " addresses");
    }

    //  A decoder as filters: a FIR-E matrix loaded, its outputs on the first channels, the rest silent, the input's
    //  normalisation switch heard, the path kept through the host, and a missing file said so.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 8));
        proc.prepareToPlay(kCheckRate, 128);
        juce::String error;
        const bool loaded =
            proc.loadMatrixFile(juce::File(std::string(BAMBI_CORE_DIR) + "/tests/decode/made-matrix.sofa"), &error);
        check(loaded && proc.decoderState().matrixOutputs == 3 && proc.decoderState().layout.empty(),
              "a filter matrix loads into the state: " + error + proc.matrixNote());
        proc.playOutput(BambiDecoderProcessor::Output::Headphones, false);
        proc.playOutput(BambiDecoderProcessor::Output::Speakers, true);
        PluginState s{decodeParams()};
        s.decoder = proc.decoderState();
        //  Catches: the matrix never reaching the audio thread, its outputs on the wrong channels, the layout's
        //  (empty) design playing instead.
        const Channels y = render(busIn(16, 8), 128, 0.25, {}, &s);
        bool on = !y.empty();
        for (int c : {0, 1, 2}) on = on && channelEnergy(y, c) > 1e-6;
        check(on, "the matrix plays on channels 1-3");
        bool rest = !y.empty();
        for (int c = 3; c < 8; ++c) rest = rest && channelEnergy(y, c) == 0.0;
        check(rest, "channels 4-8 are silent: the matrix has three outputs");
        PluginState sn3d = s;
        sn3d.decoder.matrixNormalization = 1;
        const Channels y2 = render(busIn(16, 8), 128, 0.25, {}, &sn3d);
        check(!y2.empty() && !identical(y, y2) && channelEnergy(y2, 0) < channelEnergy(y, 0),
              "sn3d on the input scales the higher orders down against n3d");
        juce::MemoryBlock saved;
        proc.getStateInformation(saved);
        BambiDecoderProcessor other;
        other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        check(other.decoderState().matrixFile == s.decoder.matrixFile && other.decoderState().matrixOutputs == 3,
              "the matrix's path survives a save and a load");
        PluginState gone = s;
        gone.decoder.matrixFile = "/nowhere/iko.sofa";
        BambiDecoderProcessor missing;
        missing.setBusesLayout(busIn(16, 8));
        missing.prepareToPlay(kCheckRate, 128);
        missing.commitDocument(gone, true);
        check(missing.designError().contains("not there"), "a missing file is said so: " + missing.designError());
        const Channels silent = render(busIn(16, 8), 128, 0.1, {}, &gone);
        check(!silent.empty() && energy(silent) == 0.0, "and the speakers stay silent rather than wrong");
    }

    //  A SOFA file: read, fitted, put into the state as a head, heard, and kept through the host without the file.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        juce::String error;
        const bool loaded =
            proc.loadSofaFile(juce::File(std::string(BAMBI_CORE_DIR) + "/tests/decode/made.sofa"), &error);
        check(loaded && proc.builtHead() == 2, "a sofa file is read, fitted and built: " + error + proc.sofaNote());
        const DecoderState with = proc.decoderState();
        check(with.hrtf == 2 && !with.hrtfFileHead.empty() && with.hrtfFile.find("made.sofa") != std::string::npos,
              "the file's head travels in the state as text");
        //  Catches: a file head that never reaches the chain, or a state whose head text is not what plays.
        PluginState s{decodeParams()};
        s.decoder = with;
        const Channels model = render(busIn(16, 2), 128, 0.25, {}), file = render(busIn(16, 2), 128, 0.25, {}, &s);
        check(!model.empty() && !file.empty() && !identical(model, file), "the file's head is heard, not the model");
        juce::MemoryBlock saved;
        proc.getStateInformation(saved);
        BambiDecoderProcessor other;
        other.setBusesLayout(busIn(16, 2));
        other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        other.prepareToPlay(kCheckRate, 128);
        check(other.decoderState().hrtfFileHead == with.hrtfFileHead && other.builtHead() == 2,
              "the head survives a save and a load, and plays without the file");
        check(!proc.loadSofaFile(juce::File(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json"), &error) &&
                  error.isNotEmpty(),
              "a file that is not SOFA is refused with a reason: " + error);
    }

    return checkVerdict();
}
