// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/decode/layouts.hpp"
#include "bambi/editor/AllocationCheck.h"
#include "bambi/editor/CheckKit.h"
#include "bambi/editor/HeaderChecks.h"
#include "bambi/host/LinkNode.h"
#include "bambi/link/link.hpp"
#include "bambi/math/sh.hpp"

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

/*  An impulse in W through a prepared processor: the first frame on which `channel` (counted from 0) is heard,
    or -1 when it stays silent over `blocks` blocks of 128. */
int firstHeard(BambiDecoderProcessor& proc, int width, int channel, int blocks) {
    juce::AudioBuffer<float> buffer(width, 128);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
        buffer.clear();
        if (b == 0) buffer.setSample(0, 0, 1.0f);
        proc.processBlock(buffer, midi);
        const float* y = buffer.getReadPointer(channel);
        for (int i = 0; i < 128; ++i)
            if (std::abs(y[i]) > 1e-7f) return b * 128 + i;
    }
    return -1;
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
        //  where the head was not. Static holds the scene still whatever the sliders say.
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

    //  A typed layout: three loudspeakers ahead do not surround the listener and are refused; a fourth behind, added
    //  as the table adds one, makes a layout that plays on its channels. Catches: the design not rebuilt on an
    //  edit of the layout, the hull check gone, a new row's channel not the next free one.
    {
        PluginState s{decodeParams()};
        s.decoder.layoutTyped = true;
        s.decoder.layoutName = "typed";
        s.decoder.speakersPlay = true;
        s.decoder.headphonesPlay = false;
        for (double az : {-30.0, 0.0, 30.0}) {
            Loudspeaker l;
            l.azimuthDeg = az;
            l.channel = static_cast<int>(s.decoder.layout.size()) + 1;
            s.decoder.layout.push_back(l);
        }
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 8));
        proc.prepareToPlay(kCheckRate, 128);
        loadState(proc, s);
        check(!proc.designError().isEmpty(), "three loudspeakers ahead are refused: " + proc.designError());
        Loudspeaker behind;
        behind.azimuthDeg = 180.0;
        behind.channel = 4;
        s.decoder.layout.push_back(behind);
        const Channels y = render(busIn(16, 8), 128, 0.25, {}, &s);
        check(!y.empty() && channelEnergy(y, 3) > 1e-4 && channelEnergy(y, 0) > 1e-4 && channelEnergy(y, 4) == 0.0,
              "with a fourth behind, the typed layout plays on channels 1-4");
    }

    //  The energy picture: a window that asks gets the field as it arrived, and nothing added -- a monitor adds
    //  nothing. Catches: the field never gathered, the picture published without a window asking, the field
    //  gathered as what was added.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        juce::AudioBuffer<float> buffer(16, 128);
        juce::MidiBuffer midi;
        std::vector<double> y(16, 0.0);
        bambi::shSN3D(bambi::fromAzEl(0.5, 0.2), 3, y);
        std::vector<float> added, arrived;
        const auto run = [&](int blocks) {
            bool landed = false;
            for (int b = 0; b < blocks; ++b) {
                for (int c = 0; c < 16; ++c) {
                    auto* x = buffer.getWritePointer(c);
                    for (int i = 0; i < 128; ++i)
                        x[i] = static_cast<float>(y[static_cast<std::size_t>(c)] * 0.5 *
                                                  std::sin(6.2831853 * 220.0 * (b * 128 + i) / kCheckRate));
                }
                proc.processBlock(buffer, midi);
                landed = proc.takeCovariances(added, arrived) || landed;
            }
            return landed;
        };
        check(!run(40), "nothing is published while no window asks for the picture");
        proc.wantEnergy(true);
        const bool landed = run(40);
        double arrivedEnergy = 0.0, addedEnergy = 0.0;
        for (float v : arrived) arrivedEnergy += std::abs(v);
        for (float v : added) addedEnergy += std::abs(v);
        check(landed && arrivedEnergy > 1e-6 && addedEnergy == 0.0,
              "asked for, the picture is the field as it arrived and nothing added");
    }

    //  One latency for every path: the array's feeds land exactly the reported latency late, and the convolved
    //  paths -- headphones, through the layout, a filter matrix -- no earlier. Catches: the array path without the
    //  partition's delay, a partition change without the report, a path early against the others.
    {
        const auto latencyOf = [&](const PluginState& s, int width, int channel) {
            BambiDecoderProcessor proc;
            proc.setBusesLayout(busIn(16, width));
            proc.prepareToPlay(kCheckRate, 128);
            loadState(proc, s);
            return std::pair{firstHeard(proc, 16, channel, 8), proc.getLatencySamples()};
        };
        PluginState pair = withLayout("0+2+0", true, false);
        pair.decoder.speakerFirstChannel = 3;
        const auto array = latencyOf(pair, 4, 2);
        check(array.first == array.second && array.second > 0,
              "the array's feed is heard exactly the reported latency late: " + juce::String(array.first) + " of " +
                  juce::String(array.second));
        const auto headphones = latencyOf(withLayout("0+2+0", false, true), 2, 0);
        check(headphones.first >= headphones.second,
              "headphones no earlier than the latency: " + juce::String(headphones.first));
        PluginState through = withLayout("0+5+0", false, true);
        through.decoder.throughLayout = true;
        const auto virtualArray = latencyOf(through, 2, 0);
        check(virtualArray.first >= virtualArray.second,
              "through the layout no earlier than the latency: " + juce::String(virtualArray.first));
    }

    //  The headphones' pair past the bus edge is silence with no mismatch counted; a stereo input, order 0, still
    //  renders; a bus narrowed after prepare renders silence and counts a mismatch. Catches: the pair's edge guard
    //  dropped, order 0 refused by the render, the ready guard dropped.
    {
        PluginState far = withLayout("0+2+0", false, true, 13);
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 8));
        proc.prepareToPlay(kCheckRate, 128);
        loadState(proc, far);
        check(firstHeard(proc, 16, 6, 4) < 0 && proc.diagnostics().layoutMismatches.load() == 0,
              "headphones placed past an eight-channel bus play nothing and count no mismatch");
        const Channels stereo = render(busIn(2, 2), 128, 0.1, {});
        bool finite = !stereo.empty();
        for (const auto& ch : stereo)
            for (float v : ch) finite = finite && std::isfinite(v);
        check(finite && !stereo.empty() && channelEnergy(stereo, 0) > 0.0,
              "a stereo input, order 0, renders finite sound on the pair");
        BambiDecoderProcessor narrowed;
        narrowed.setBusesLayout(busIn(16, 2));
        narrowed.prepareToPlay(kCheckRate, 128);
        narrowed.setBusesLayout(busIn(4, 2));
        check(firstHeard(narrowed, 16, 0, 4) < 0 && narrowed.diagnostics().layoutMismatches.load() > 0,
              "a bus narrowed after prepare renders silence and counts the mismatch");
    }

    //  Determinism: the same render twice, and however the host cuts its blocks.
    {
        const PluginState s = withLayout("4+7+0", true, true, 13);
        const double seconds = 512.0 * 48 / kCheckRate;  // a whole number of every block size tried
        const Channels a = render(busIn(16, 18), 128, seconds, {}, &s), b = render(busIn(16, 18), 128, seconds, {}, &s);
        check(identical(a, b), "two renders are bit-identical");
        const Channels c = render(busIn(16, 18), 64, seconds, {}, &s), d = render(busIn(16, 18), 512, seconds, {}, &s);
        check(identical(a, c) && identical(a, d), "the block size does not change a sample: " + firstDifference(a, c));
        //  Catches: the scene's turn set once a block instead of per rotation hop, which a turning scene would show.
        const auto spinning = [](BambiDecoderProcessor& p) { setParameter(p, "head.yaw_rate", 90.0f); };
        const Channels e = render(busIn(16, 18), 64, seconds, spinning, &s),
                       f = render(busIn(16, 18), 512, seconds, spinning, &s);
        check(!e.empty() && !identical(a, e) && identical(e, f),
              "a turning scene renders the same at every block size: " + firstDifference(e, f));
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

    //  The port: bound on the loopback alone, or on every interface when opened to the network, and handed to the
    //  receiver; closed again when no source wants it. Catches: the socket bound but never handed over, the port
    //  not reopened when the flag changes, the port left open with no tracker.
    {
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 2));
        proc.prepareToPlay(kCheckRate, 128);
        const int port = 19000 + static_cast<int>(juce::Random::getSystemRandom().nextInt(1000));
        proc.editDecoder("osc", [port](DecoderState& d) {
            d.trackerSource = 1;
            d.oscPort = port;
            d.oscNetwork = false;
        });
        check(proc.trackerPortOpen(), "choosing osc binds the port and the receiver reads it: " + juce::String(port));
        proc.editDecoder("network", [](DecoderState& d) { d.oscNetwork = true; });
        check(proc.trackerPortOpen(), "opened to the network, the port is bound again on every interface");
        proc.editDecoder("none", [](DecoderState& d) { d.trackerSource = 0; });
        check(!proc.trackerPortOpen(), "and closed when no source wants it");
        juce::DatagramSocket probe(false);
        check(probe.bindToPort(port, "127.0.0.1"), "so the port is free again");
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

    //  Two decoders in one session: each sees the other; the window's switch lists both; choosing the other follows
    //  it, with the output squares still the other's own to play; and back. The other's decoder state does not
    //  travel the bus (the link's patch record carries the effects' patch), so what its page shows is not checked
    //  here. Catches: the switch not fed from the scene, the follow never taken, the squares playing a remote
    //  output, the shown instance not returning to this one.
    {
        std::printf("\ntwo decoders\n");
        //  A private session directory: the machine's real one may belong to a running DAW session.
        bambi::host::setLinkDirectoryNameForTests("bambi-decoder-check-" +
                                                  bambi::Uuid::generate().toString().substr(0, 8));
        BambiDecoderProcessor one, two;
        for (auto* p : {&one, &two}) {
            p->setBusesLayout(busIn(16, 18));
            p->prepareToPlay(kCheckRate, 128);
        }
        //  Ticked by hand: a headless check has no message loop to run the timer.
        const auto tick = [&](int ticks) {
            for (int i = 0; i < ticks; ++i)
                for (auto* p : {&one, &two}) p->linkTick();
        };
        tick(bambi::host::LinkNode::kTickHz / 10 + 4);
        check(one.linkStatus().open && two.linkStatus().open, "two decoders join the bus");
        check(one.identity().session == two.identity().session && !(one.identity().instance == two.identity().instance),
              "in one session, with ids of their own");
        two.renameInstance("other");
        loadState(two, withLayout("4+5+0", true, false));
        loadState(one, withLayout("4+7+0", true, false));
        tick(4);
        std::unique_ptr<juce::AudioProcessorEditor> editor(one.createEditor());
        auto* window = dynamic_cast<BambiDecoderEditor*>(editor.get());
        check(window != nullptr, "the first opens its window");
        if (window != nullptr) {
            window->tick();
            window->paintNow();
            check(window->sessionDecoders() == 2,
                  "whose switch lists two decoders: " + juce::String(window->sessionDecoders()));
            check(!window->instanceSwitchArea(1).isEmpty(), "drawn in the header, since there are two");
            check(!window->showingAnother() && window->shownLayoutName() == "4+7+0", "showing its own at first");
            window->showDecoder(1);
            for (int i = 0; i < 6; ++i) {
                tick(2);
                window->tick();
            }
            window->paintNow();
            check(window->showingAnother(), "choosing the other follows it");
            const bool played = two.decoderState().speakersPlay;
            window->clickOutputSquare(1);
            window->tick();
            check(two.decoderState().speakersPlay == played, "its square is not this window's to play");
            window->showDecoder(0);
            window->tick();
            window->paintNow();
            check(!window->showingAnother() && window->shownLayoutName() == "4+7+0", "and back to its own");
        }
        editor.reset();
        bambi::LinkBus::unlinkSession(one.identity().session);
    }

    //  The globe's sources toggle and its list: what the window draws of the other instances on the bus, and the boxes that
    //  switch each one off. Driven through the painted areas as the mouse is, on a decoder with Encoders on its own bus.
    {
        std::printf("\nthe sources list\n");
        bambi::host::setLinkDirectoryNameForTests("bambi-decoder-check-" +
                                                  bambi::Uuid::generate().toString().substr(0, 8));
        BambiDecoderProcessor dec;
        dec.setBusesLayout(busIn(16, 18));
        dec.prepareToPlay(kCheckRate, 128);
        const auto tick = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) dec.linkTick();
        };
        tick(bambi::host::LinkNode::kTickHz / 10 + 4);

        std::vector<std::unique_ptr<bambi::LinkBus>> sources;
        const auto addSource = [&](const std::string& name, double azimuthDeg) {
            auto bus = std::make_unique<bambi::LinkBus>();
            if (!bus->open(dec.identity().session, bambi::Uuid::generate(), bambi::Product::Encoder)) return;
            bambi::LinkStatic st;
            st.setLabel(name);
            bus->publishStatic(st);
            bambi::LinkDynamic d;
            const bambi::Vec3 at = bambi::fromAzEl(azimuthDeg * bambi::kDeg2Rad, 0.1);
            d.x = static_cast<float>(at.x);
            d.y = static_cast<float>(at.y);
            d.z = static_cast<float>(at.z);
            bus->publishDynamic(d);
            sources.push_back(std::move(bus));
        };

        std::unique_ptr<juce::AudioProcessorEditor> editor(dec.createEditor());
        auto* window = dynamic_cast<BambiDecoderEditor*>(editor.get());
        check(window != nullptr, "the window opens");
        if (window != nullptr) {
            const auto settle = [&] {
                tick(4);
                window->tick();
                window->paintNow();
            };
            settle();
            //  Catches: a toggle and a triangle for an empty bus, which would open a list with nothing in it.
            window->clickSourcesTriangle();
            check(window->sourcesListed() == 0 && !window->sourcesMenuOpen(),
                  "with nothing on the bus the list has no rows and the triangle opens nothing");

            addSource("kick", -80.0);
            addSource("bass", 0.0);
            addSource("pad", 80.0);
            settle();
            check(window->sourcesListed() == 3 && window->sourcesDrawn() == 3,
                  "three Encoders are listed and drawn: " + juce::String(window->sourcesListed()) + " listed, " +
                      juce::String(window->sourcesDrawn()) + " drawn");

            window->clickSourcesTriangle();
            window->paintNow();
            check(window->sourcesMenuOpen() && !window->sourcesMenuBox().isEmpty() &&
                      !window->sourcesRowArea(0).isEmpty() && !window->sourcesRowArea(2).isEmpty(),
                  "the triangle opens the list, a row for each");
            window->clickSourcesTriangle();
            check(!window->sourcesMenuOpen(), "and a second press on it closes the list");
            //  Catches: Escape ignored by an open list, as the sheets and the preset browser do not; or taken with no list
            //  open, which would eat a key the window or the host wants.
            const bool escapeIgnored = window->pressKeyOnGlobe(juce::KeyPress(juce::KeyPress::escapeKey));
            window->clickSourcesTriangle();
            const bool escapeTaken = window->pressKeyOnGlobe(juce::KeyPress(juce::KeyPress::escapeKey));
            check(!escapeIgnored && escapeTaken && !window->sourcesMenuOpen(),
                  "Escape closes an open list and is not taken when none is open");
            window->clickSourcesTriangle();

            //  Catches: a row's box that is not wired to its own item, or that switches off every item.
            window->clickSourcesRow(1);
            window->paintNow();
            check(window->sourcesDrawn() == 2 && window->sourcesMenuOpen(),
                  "a row's box switches that one source off and leaves the list open");
            window->clickSourcesRow(1);
            window->paintNow();
            check(window->sourcesDrawn() == 3, "and on again");

            //  Catches: the master read only for the toggle's look, so that sources stay drawn with it off; the master
            //  woken by a tick; the choices forgotten while it is off.
            window->clickSourcesRow(0);  // kick off
            window->clickSourcesToggle();
            window->paintNow();
            check(!window->sourcesMasterOn() && window->sourcesDrawn() == 0 && window->sourcesListed() == 3,
                  "the toggle is the master: off draws nothing, and the list still names every one");
            window->clickSourcesRow(2);  // pad ticked off while the master is off
            window->clickSourcesRow(2);  // and on again
            window->paintNow();
            check(!window->sourcesMasterOn() && window->sourcesDrawn() == 0,
                  "a tick while the master is off leaves the master alone");
            window->clickSourcesToggle();
            window->paintNow();
            check(window->sourcesMasterOn() && window->sourcesDrawn() == 2,
                  "switched back on, the choices are as they were: kick stays off");

            //  Catches: all and none swapped, or reaching sources that are not listed.
            window->clickSourcesAll(false);
            window->paintNow();
            const int afterNone = window->sourcesDrawn();
            window->clickSourcesAll(true);
            window->paintNow();
            check(afterNone == 0 && window->sourcesDrawn() == 3, "none switches every source off, all every one on");

            //  Catches: a press on the list that starts an orbit under it, a press elsewhere that does not close it, and
            //  the closing press that orbits as it closes.
            const double yaw = window->globeCameraYaw();
            const auto row = window->sourcesRowArea(1).getCentre();
            window->dragGlobeBetween(row, row + juce::Point<float>(60.0f, 30.0f));
            check(window->sourcesMenuOpen() && std::abs(window->globeCameraYaw() - yaw) < 1e-12,
                  "a drag that starts on the list orbits nothing");
            const juce::Point<float> away{250.0f, 250.0f};
            window->dragGlobeBetween(away, away + juce::Point<float>(30.0f, 0.0f));
            check(!window->sourcesMenuOpen() && std::abs(window->globeCameraYaw() - yaw) < 1e-12,
                  "a press elsewhere closes the list, and does not orbit as it does");
            const juce::Point<float> middle{142.0f, 170.0f};  // the globe's own middle, the list now closed
            window->dragGlobeBetween(middle, middle + juce::Point<float>(30.0f, 0.0f));
            check(std::abs(window->globeCameraYaw() - yaw) > 1e-6, "and the next drag orbits as it did");

            //  Catches: choices kept for sources that have left, or a source that joins arriving switched off.
            const int choicesBefore = window->sourcesChoices();
            sources.pop_back();  // pad leaves the bus
            for (int i = 0; i < 6; ++i) {
                tick(2);
                window->tick();
            }
            window->paintNow();
            check(window->sourcesListed() == 2 && window->sourcesDrawn() == 2, "a source that leaves leaves the list");
            check(choicesBefore == 3 && window->sourcesChoices() == 2,
                  "and takes its choice with it: " + juce::String(choicesBefore) + " choices, now " +
                      juce::String(window->sourcesChoices()));
            addSource("vox", 160.0);
            settle();
            check(window->sourcesListed() == 3 && window->sourcesDrawn() == 3,
                  "and one that joins is drawn, whatever was chosen before");

            //  Catches: a long list that cannot be reached: more rows than the list shows, with nowhere to scroll to.
            for (int i = 0; i < 12; ++i) addSource("s" + std::to_string(i), i * 25.0);
            settle();
            window->clickSourcesTriangle();
            window->paintNow();
            check(window->sourcesListed() == 15 && window->sourcesMenuAreas().maxScroll > 0.0f &&
                      window->sourcesRowArea(14).isEmpty() && !window->sourcesRowArea(0).isEmpty(),
                  "a long list shows part of itself and can scroll for the rest");
        }
        editor.reset();
        sources.clear();
        bambi::LinkBus::unlinkSession(dec.identity().session);
    }

    //  What the Decoder publishes for every other plugin's scene: the layout while it plays one, what it plays, and where its
    //  head looks as it resolved it. Read from outside, on the bus, as another instance's window reads it.
    {
        std::printf("\nwhat the Decoder publishes for other scenes\n");
        bambi::host::setLinkDirectoryNameForTests("bambi-decoder-check-" +
                                                  bambi::Uuid::generate().toString().substr(0, 8));
        BambiDecoderProcessor dec;
        dec.setBusesLayout(busIn(16, 18));
        dec.prepareToPlay(kCheckRate, 128);
        const auto tick = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) dec.linkTick();
        };
        tick(bambi::host::LinkNode::kTickHz / 10 + 4);
        bambi::LinkBus viewer;
        const bool joined = viewer.open(dec.identity().session, bambi::Uuid::generate(), bambi::Product::Encoder);
        check(joined, "a viewer joins the Decoder's session");

        const auto runBlocks = [&] {
            juce::AudioBuffer<float> buffer(18, 128);
            juce::MidiBuffer midi;
            for (int b = 0; b < 6; ++b) {
                buffer.clear();
                dec.processBlock(buffer, midi);
                tick(2);
            }
        };
        const auto read = [&](bambi::LinkScene& scene) -> const bambi::LinkScene::Entry* {
            scene.update(viewer);
            for (const auto& e : scene.entries())
                if (e.instance == dec.identity().instance) return &e;
            return nullptr;
        };

        PluginState s = withLayout("4+7+0", true, true, 13);
        s.decoder.trackerSource = 1;  // a tracker over OSC turns the head; the sliders turn the scene
        loadState(dec, s);
        dec.takeOrientation("/ypr", yprValues(90.0f));  // turned to the left
        runBlocks();
        tick(4);
        bambi::LinkScene scene;
        const auto* entry = read(scene);
        check(entry != nullptr && entry->product == bambi::Product::Decoder && !entry->hasPosition,
              "it is on the bus as a Decoder, with no source position");
        if (entry != nullptr) {
            //  Catches: the flags not published, or published from the wrong state.
            check(
                entry->dyn.playFlags == (bambi::kLinkPlaySpeakers | bambi::kLinkPlayHeadphones),
                "it publishes that it plays loudspeakers and headphones: flags " + juce::String(entry->dyn.playFlags));
            //  Catches: the layout dropped from the static section, or cut.
            check(entry->st.speakerCount == s.decoder.layout.size(),
                  "its layout arrives whole: " + juce::String(static_cast<int>(entry->st.speakerCount)) + " of " +
                      juce::String(static_cast<int>(s.decoder.layout.size())) + " points");
            //  Catches: the head published as the front, or in the wrong frame -- a head turned left looks along +y.
            const bambi::Vec3 gaze = entry->dyn.headGaze(), left = entry->dyn.headLeft();
            check(std::abs(gaze.y - 1.0) < 1e-3 && std::abs(gaze.x) < 1e-3 && std::abs(left.x + 1.0) < 1e-3,
                  "its head looks where it is turned: gaze (" + juce::String(gaze.x, 2) + ", " +
                      juce::String(gaze.y, 2) + ", " + juce::String(gaze.z, 2) + ")");
        }

        //  Catches: a layout still published after the Decoder stops playing it, or a head after it stops playing headphones.
        loadState(dec, withLayout("4+7+0", false, true, 13));
        runBlocks();
        tick(4);
        entry = read(scene);
        check(entry != nullptr && entry->dyn.playFlags == bambi::kLinkPlayHeadphones && entry->st.speakerCount == 0,
              "on headphones alone it publishes no layout and no loudspeaker flag");
        bambi::LinkBus::unlinkSession(dec.identity().session);
    }

    std::printf("\nthe audio thread allocates nothing, on a bus of 36 or 64 channels\n");
    bambi::editor::checkBlocksAllocateNothing<BambiDecoderProcessor>(busIn(36, 2), "36 in, headphones out");
    bambi::editor::checkBlocksAllocateNothing<BambiDecoderProcessor>(busIn(64, 64), "64 in, 64 out");

    bambi::editor::checkHeaderHoldsStill<BambiDecoderProcessor>(bambi::Product::Decoder);

    bambi::editor::checkBypass<BambiDecoderProcessor>(busIn(16, 2), "silence", [](int, juce::int64) { return 0.0f; });

    return checkVerdict();
}
