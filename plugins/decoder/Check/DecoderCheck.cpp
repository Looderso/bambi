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
        //  Lighting headphones on 1-2 beside the layout stops the layout: the same channels.
        BambiDecoderProcessor proc;
        proc.setBusesLayout(busIn(16, 18));
        proc.commitDocument(withLayout("4+7+0", true, false, 1), true);
        proc.playOutput(BambiDecoderProcessor::Output::Headphones, true);
        const DecoderState after = proc.decoderState();
        check(after.headphonesPlay && !after.speakersPlay && proc.notice().contains("speakers stopped"),
              "headphones on 1-2 stop the layout and say so: " + proc.notice());
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
