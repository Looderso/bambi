// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-plugin-check — the host-contract properties that need no host.
//
//  A REAPER session can only show what REAPER does. What the plugin promises regardless of host is
//  checked here, on the shipped sources, before anyone opens a DAW:
//
//    - which bus layouts it accepts, including the exact requests REAPER's AU hosting made
//    - that a wide input changes nothing: the source is channels 1-2 in every format
//    - that output does not depend on the host's block size -- realtime playback and an offline
//      bounce use different ones, so this is the bounce check, bit for bit
//
//  exit 0 every check passes, 1 a check failed

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/FrameTrace.h"
#include "bambi/editor/CheckKit.h"
#include "bambi/editor/PresetChecks.h"
#include "bambi/editor/RegionChecks.h"
#include "bambi/encode/params.hpp"
#include "bambi/host/TestPlayHead.h"
#include "bambi/link/directory.hpp"
#include "bambi/link/link.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/mod/sources.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/generator.hpp"

namespace {
constexpr double kRate = bambi::editor::kCheckRate;

using bambi::editor::Channels;
using bambi::editor::check;
using bambi::editor::checkFailures;
using bambi::editor::checkVerdict;
using bambi::editor::describe;
using bambi::editor::findParam;
using bambi::editor::firstDifference;
using bambi::editor::Layout;
using bambi::editor::layout;
using bambi::editor::peak;
using bambi::editor::setParameter;

struct TimedNote {
    juce::int64 sample;
    juce::MidiMessage message;
};

using PlayingHead = bambi::host::TestPlayHead;

/*  A processor constructed inside this scope reports `type` as its wrapperType, as it does when a
    wrapper creates it. Which main input widths are accepted depends on the format, so every check
    that negotiates a layout has to say which format it stands for. CLAP reports Undefined. */
struct AsFormat {
    explicit AsFormat(juce::AudioProcessor::WrapperType type) { juce::AudioProcessor::setTypeOfNextNewPlugin(type); }
    ~AsFormat() { juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined); }
    AsFormat(const AsFormat&) = delete;
    AsFormat& operator=(const AsFormat&) = delete;
};

/*  What JUCE's VST3 wrapper settles on when the plugin refuses the arrangement a host asks for
    (setBusArrangements): it walks the buses from last to first, input before output, and keeps what
    each bus may take from the layout built so far. The same walk, on the real processor. */
Layout settleLikeVst3(const juce::AudioProcessor& proc, const Layout& requested) {
    if (proc.checkBusesLayoutSupported(requested)) return requested;
    auto settled = proc.getBusesLayout();
    for (int bus = std::max(proc.getBusCount(true), proc.getBusCount(false)) - 1; bus >= 0; --bus)
        for (const bool isInput : {true, false})
            if (const auto* b = proc.getBus(isInput, bus))
                b->isLayoutSupported(requested.getChannelSet(isInput, bus), &settled);
    return settled;
}

/*  Render `seconds` of a deterministic source -- a function of the sample index, so identical in
    every run -- through a fresh processor. Channels 3 and up of a wide main input can be filled
    with noise, to prove they are ignored. Returns the main output's channels. */
Channels render(const Layout& l, int block, double seconds, bool noiseInExtraInputs, const std::string& state = {},
                const std::vector<TimedNote>& notes = {}, juce::int64 start = 0, bool staleHalves = false) {
    BambiEncoderProcessor proc;
    if (!proc.setBusesLayout(l)) return {};
    PlayingHead head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(kRate, block);
    if (!state.empty()) proc.setStateInformation(state.data(), static_cast<int>(state.size()));
    setParameter(proc, "motion.speed", 90.0f);
    setParameter(proc, "render.width", 30.0f);
    proc.prepareToPlay(kRate, block);

    const int mainIn = proc.getChannelCountOfBus(true, 0);
    const int sidechainIn = proc.getChannelCountOfBus(true, 1);
    const int outChannels = proc.getChannelCountOfBus(false, 0);
    const int bufferChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    const int total = static_cast<int>(seconds * kRate);

    Channels out(static_cast<std::size_t>(outChannels), std::vector<float>(static_cast<std::size_t>(total), 0.0f));
    juce::AudioBuffer<float> buffer(bufferChannels, block);
    juce::MidiBuffer midi;
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);
        buffer.setSize(bufferChannels, n, false, false, true);
        buffer.clear();
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(pos + i) / kRate;
            const float left =
                static_cast<float>(0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * t) *
                                   (0.5 + 0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 1.3 * t)));
            const float right = static_cast<float>(0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 330.0 * t));
            const float side = static_cast<float>(0.2 * std::sin(2.0 * juce::MathConstants<double>::pi * 55.0 * t));
            buffer.setSample(0, i, left);
            if (mainIn > 1) buffer.setSample(1, i, right);
            if (noiseInExtraInputs)
                for (int c = 2; c < mainIn; ++c) buffer.setSample(c, i, noise(rng));
            for (int c = 0; c < std::min(2, sidechainIn); ++c) buffer.setSample(mainIn + c, i, side);
        }
        head.time = start + pos;
        midi.clear();
        for (const auto& note : notes)
            if (note.sample >= pos && note.sample < pos + n)
                midi.addEvent(note.message, static_cast<int>(note.sample - pos));
        if (staleHalves && n > 1) {
            //  As a CLAP wrapper splits a block: two calls, both told the host block's position.
            const int first = n / 2;
            juce::AudioBuffer<float> a(buffer.getArrayOfWritePointers(), bufferChannels, 0, first);
            juce::AudioBuffer<float> b(buffer.getArrayOfWritePointers(), bufferChannels, first, n - first);
            juce::MidiBuffer none;
            proc.processBlock(a, midi);
            proc.processBlock(b, none);
        } else
            proc.processBlock(buffer, midi);
        for (int c = 0; c < outChannels; ++c)
            std::copy_n(buffer.getReadPointer(c), n, out[static_cast<std::size_t>(c)].begin() + pos);
    }
    proc.releaseResources();
    return out;
}

/// Counts what a host would see of one parameter: gestures and value changes.
struct GestureRecorder final : juce::AudioProcessorListener {
    int index{-1}, begins{0}, ends{0}, values{0};
    void audioProcessorParameterChanged(juce::AudioProcessor*, int parameterIndex, float) override {
        if (parameterIndex == index) ++values;
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int parameterIndex) override {
        if (parameterIndex == index) ++begins;
    }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int parameterIndex) override {
        if (parameterIndex == index) ++ends;
    }
};
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    using Set = juce::AudioChannelSet;

    //  Every processor in this program uses a private session directory: the machine's real one
    //  may belong to a REAPER session running right now.
    const std::string directoryName = "/bambi.pc." + bambi::Uuid::generate().toString().substr(0, 8);
    BambiEncoderProcessor::setLinkDirectoryNameForTests(directoryName);
    const auto stereo = Set::stereo();

    std::printf("\nbambi-plugin-check\n\nbus layouts\n");
    using W = juce::AudioProcessor;
    const auto vst3 = W::wrapperType_VST3, clap = W::wrapperType_Undefined;
    struct Case {
        const char* what;
        W::WrapperType as;
        Layout l;
        bool accept;
    };
    const Case cases[] = {
        {"VST3 default: in 2 | sc 2 | out 16 ambisonic", vst3, layout(stereo, stereo, Set::ambisonic(3)), true},
        {"CLAP default: in 2 | sc 2 | out 16 ambisonic", clap, layout(stereo, stereo, Set::ambisonic(3)), true},
        {"VST3 wide track: in 2 | sc 2 | out 36 (5th order)", vst3, layout(stereo, stereo, Set::discreteChannels(36)),
         true},
        {"mono in, sidechain off, 1st order", vst3, layout(Set::mono(), Set::disabled(), Set::ambisonic(1)), true},
        {"CLAP 10th order: in 2 | sc 2 | out 121", clap, layout(stereo, stereo, Set::discreteChannels(121)), true},
        {"VST3 even track: in 2 | sc 2 | out 10 (2nd order + 1 unused)", vst3,
         layout(stereo, stereo, Set::discreteChannels(10)), true},
        {"REAPER maximum: in 2 | sc 2 | out 128 (10th order + 7 unused)", vst3,
         layout(stereo, stereo, Set::discreteChannels(128)), true},
        {"stereo track: out 2 is order 0, omnidirectional", vst3, layout(stereo, stereo, stereo), true},
        //  The one bus rule, in every format. A main input wider than stereo lets a 16-channel main
        //  bus swallow a send on track channels 3/4.
        {"refused as VST3: in 16 | sc 2 | out 16, a main bus that swallows track channels 3/4", vst3,
         layout(Set::ambisonic(3), stereo, Set::ambisonic(3)), false},
        {"refused as CLAP: in 16 | sc 2 | out 16, the same wide main bus", clap,
         layout(Set::ambisonic(3), stereo, Set::ambisonic(3)), false},
        {"refused as VST3: in 36 | sc 2 | out 36, however wide the track", vst3,
         layout(Set::discreteChannels(36), stereo, Set::discreteChannels(36)), false},
        {"refused as CLAP: in 10 | sc 2 | out 10", clap,
         layout(Set::discreteChannels(10), stereo, Set::discreteChannels(10)), false},
        {"refused: no main input", vst3, layout(Set::disabled(), stereo, Set::ambisonic(3)), false},
        {"refused: no output", vst3, layout(stereo, stereo, Set::disabled()), false},
    };
    for (const auto& c : cases) {
        const AsFormat format(c.as);
        BambiEncoderProcessor proc;
        const bool accepted = proc.setBusesLayout(c.l);
        check(accepted == c.accept, juce::String(c.what) + (accepted ? "  -> accepted" : "  -> refused"));
    }

    std::printf("\nwhat JUCE's VST3 wrapper settles on for a track wider than stereo\n");
    {
        /*  REAPER asks a VST3 for as many inputs as the track has channels. That is refused, and the
            wrapper falls back bus by bus: the output must still follow the track, and the main input must
            stay at 2, so the sidechain begins at pin 3. What REAPER then does with the kResultFalse returned
            alongside it is something only REAPER itself can show. */
        const AsFormat format(vst3);
        for (const int channels : {4, 10, 16, 26, 36, 64}) {
            BambiEncoderProcessor proc;
            const auto settled =
                settleLikeVst3(proc, layout(Set::discreteChannels(channels), stereo, Set::discreteChannels(channels)));
            const bool ok = proc.setBusesLayout(settled) && settled.getMainInputChannelSet().size() == 2 &&
                            settled.getChannelSet(true, 1).size() == 2 &&
                            settled.getMainOutputChannelSet().size() == channels;
            check(ok, juce::String(channels) + "-channel track settles on " + describe(settled));
        }
    }

    std::printf("\nthe order that fits an output width\n");
    {
        //  Oracle: the definition, by counting up. Independent of the square-root shortcut.
        int wrong = 0;
        juce::String firstWrong;
        for (int channels = 0; channels <= 130; ++channels) {
            int expected = -1;
            for (int n = 0; (n + 1) * (n + 1) <= channels; ++n)
                expected = std::min(n, BambiEncoderProcessor::kMaxHostOrder);
            const int got = bambi::host::orderThatFits(channels);
            if (got != expected && wrong++ == 0)
                firstWrong = "  (" + juce::String(channels) + " channels: " + juce::String(got) + ", expected " +
                             juce::String(expected) + ")";
        }
        check(wrong == 0,
              "0-130 channels match the definition: 4 -> 1, 10 -> 2, 26 -> 4, 36 -> 5, 128 -> 10" + firstWrong);
    }

    std::printf("\nthe source is the main input's channels, summed (no format takes a wider one)\n");
    //  A wide main input is refused in every format now -- the refusals above are what guard it -- so
    //  what is left to check is the widths that are accepted.
    const auto narrow = render(layout(stereo, stereo, Set::ambisonic(3)), 512, 5.0, false);
    check(peak(narrow) > 0.01f, "the stereo render is not silent (peak " + juce::String(peak(narrow), 3) + ")");
    const auto monoIn = render(layout(Set::mono(), stereo, Set::ambisonic(3)), 512, 5.0, false);
    check(peak(monoIn) > 0.01f, "the mono render is not silent either (peak " + juce::String(peak(monoIn), 3) + ")");

    std::printf("\nchannels beyond the order are silent\n");
    const auto nine = render(layout(stereo, stereo, Set::ambisonic(2)), 512, 5.0, false);
    //  in 2 | out 10 is what VST3 settles on for a 10-channel track; the property is about the
    //  output, so it needs no wide input.
    const auto ten = render(layout(stereo, stereo, Set::discreteChannels(10)), 512, 5.0, false);
    check(peak(nine) > 0.01f, "the 9-channel render is not silent (peak " + juce::String(peak(nine), 3) + ")");
    const bool lastSilent =
        ten.size() == 10 && std::all_of(ten[9].begin(), ten[9].end(), [](float v) { return v == 0.0f; });
    check(lastSilent, "in 2 | out 10: output channel 10 is exactly zero");
    const Channels firstNine(ten.begin(),
                             ten.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(9, ten.size())));
    const auto nineDiff = firstDifference(nine, firstNine);
    check(nineDiff.isEmpty(), "its first 9 channels are bit-identical to a 9-channel render" +
                                  (nineDiff.isEmpty() ? juce::String() : "  (" + nineDiff + ")"));
    const auto omni = render(layout(stereo, stereo, stereo), 512, 2.0, false);
    bool omniFinite = !omni.empty();
    for (const auto& ch : omni)
        omniFinite = omniFinite && std::all_of(ch.begin(), ch.end(), [](float v) { return std::isfinite(v); });
    const bool omniShape =
        omni.size() == 2 && std::all_of(omni[1].begin(), omni[1].end(), [](float v) { return v == 0.0f; });
    check(omniFinite && omniShape && peak(omni) > 0.01f,
          "2 out is order 0: W carries the source (peak " + juce::String(peak(omni), 3) + "), channel 2 silent");

    std::printf("\noutput does not depend on the host's block size (the offline-bounce property)\n");
    const auto base = render(layout(stereo, stereo, Set::ambisonic(3)), 512, 10.0, false);
    const auto again = render(layout(stereo, stereo, Set::ambisonic(3)), 512, 10.0, false);
    const auto againDiff = firstDifference(base, again);
    check(againDiff.isEmpty(),
          "block 512, rendered twice, is identical" + (againDiff.isEmpty() ? juce::String() : "  (" + againDiff + ")"));
    //  Block 1 is the pathological case: control runs on a fixed 256-sample grid, so a host that
    //  calls one sample at a time exercises every partial step there is. Ten seconds at block 1 is
    //  slow, so it renders two, which is still 96,000 calls.
    for (int block : {64, 128, 480, 1024, 2048}) {
        const auto other = render(layout(stereo, stereo, Set::ambisonic(3)), block, 10.0, false);
        const auto diff = firstDifference(base, other);
        check(diff.isEmpty(), "block " + juce::String(block) + " renders bit-identical to block 512" +
                                  (diff.isEmpty() ? juce::String() : "  (" + diff + ")"));
    }
    {
        const auto shortBase = render(layout(stereo, stereo, Set::ambisonic(3)), 512, 2.0, false);
        const auto single = render(layout(stereo, stereo, Set::ambisonic(3)), 1, 2.0, false);
        const auto diff = firstDifference(shortBase, single);
        check(diff.isEmpty(),
              "block 1 renders bit-identical to block 512" + (diff.isEmpty() ? juce::String() : "  (" + diff + ")"));
    }

    std::printf("\na synced LFO does not depend on where the host begins a block\n");
    {
        //  LFO 1 synced at 1/16 on yaw, a yaw rate beside it, from a start that is not on the grid:
        //  a step's song position rounds differently if it is taken from the block's start.
        bambi::PluginState patch{bambi::encodeParams()};
        bambi::generatorDefaults(patch.trajectory.generator, patch.trajectory.genParams);
        patch.params[static_cast<std::size_t>(bambi::EncoderParam::Lfo1Div)] = 14.0f;
        patch.params[static_cast<std::size_t>(bambi::EncoderParam::TransformYawRate)] = 37.0f;
        patch.matrix.push_back({bambi::MatrixTab::Generators, 0, bambi::EncoderParam::TransformYaw, 1.0});
        const std::string patchText = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), patch);
        const auto l = layout(stereo, stereo, Set::ambisonic(3));
        const juce::int64 start = 123457;
        const auto base = render(l, 1000, 2.0, false, patchText, {}, start);
        for (int block : {1, 64}) {
            const auto diff = firstDifference(base, render(l, block, 2.0, false, patchText, {}, start));
            check(diff.isEmpty(), "from sample 123457, block " + juce::String(block) +
                                      " renders bit-identical to block 1000" +
                                      (diff.isEmpty() ? juce::String() : "  (" + diff + ")"));
        }
        const auto split = firstDifference(base, render(l, 1000, 2.0, false, patchText, {}, start, true));
        check(split.isEmpty(), "sub-blocks told their host block's position render bit-identical to whole blocks" +
                                   (split.isEmpty() ? juce::String() : "  (" + split + ")"));
    }

    {
        //  Stopped, the song does not move, so neither does a synced LFO -- whatever the grid counts.
        bambi::PluginState patch{bambi::encodeParams()};
        bambi::generatorDefaults(patch.trajectory.generator, patch.trajectory.genParams);
        patch.params[static_cast<std::size_t>(bambi::EncoderParam::Lfo1Div)] = 14.0f;
        patch.matrix.push_back({bambi::MatrixTab::Generators, 0, bambi::EncoderParam::TransformYaw, 1.0});
        const std::string patchText = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), patch);
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.setStateInformation(patchText.data(), static_cast<int>(patchText.size()));
        proc.prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        const auto yawAfter = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                buffer.clear();
                proc.processBlock(buffer, midi);
                if (head.playing) head.time += 512;
            }
            for (int i = 0; i < BambiEncoderProcessor::kLinkTickHz / 10 + 2; ++i) proc.linkTick();
            bambi::LinkScene sc;
            proc.updateScene(sc);
            const auto* self = sc.find(proc.identity().instance);
            return self != nullptr ? self->dyn.yawRad : -999.0;
        };
        head.time = 30011;
        yawAfter(4);  // playing: the LFO is somewhere
        head.playing = false;
        const double first = yawAfter(60);  // yaw is slewed: the ramp into where it stopped
        const double later = yawAfter(40);
        check(first > -999.0 && first == later, "stopped, a synced LFO holds where the song stopped (" +
                                                    juce::String(first * bambi::kRad2Deg, 3) + " vs " +
                                                    juce::String(later * bambi::kRad2Deg, 3) + " deg)");
    }

    std::printf("\nthe editor's edits: saved, undoable, and heard\n");
    {
        BambiEncoderProcessor proc;
        const auto saved = [&proc] {
            juce::MemoryBlock block;
            proc.getStateInformation(block);
            bambi::PluginState s{bambi::encodeParams()};
            bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                             std::string_view(static_cast<const char*>(block.getData()), block.getSize()), s);
            return s;
        };
        const auto depthTo = [](double depth) {
            return [depth](bambi::PluginState& s) {
                bambi::setCellDepth(bambi::encodeMod(), s, bambi::MatrixTab::Features, 0,
                                    bambi::EncoderParam::RenderWidth, depth);
            };
        };
        const auto widthDepth = [&saved] {
            return bambi::cellDepth(saved(), bambi::MatrixTab::Features, 0, bambi::EncoderParam::RenderWidth);
        };

        proc.edit("matrix depth", depthTo(1.0));
        const bool added = widthDepth() > 0.99;
        const bool undone = proc.undoEdit() && saved().matrix.empty();
        const bool redone = proc.redoEdit() && widthDepth() > 0.99;
        check(added && undone && redone, "an edit is saved with the project, and undoes and redoes");

        for (int i = 1; i <= 20; ++i) proc.editCoalescing("matrix depth", "matrix.drag", depthTo(0.5 * i / 20.0));
        proc.endEditGesture();
        const bool dragged = std::abs(widthDepth() - 0.5) < 1e-9;
        const bool oneStep = proc.undoEdit() && widthDepth() > 0.99;
        check(dragged && oneStep, "a drag through twenty values is one undo step");

        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        proc.processBlock(buffer, midi);
        const auto before = proc.diagnostics().stateSequence.load();
        proc.edit("matrix depth", depthTo(0.25));
        proc.processBlock(buffer, midi);
        check(proc.diagnostics().stateSequence.load() > before, "an edit reaches the audio thread at the next block");

        const auto text =
            bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), bambi::PluginState{bambi::encodeParams()});
        proc.setStateInformation(text.data(), static_cast<int>(text.size()));
        check(!proc.canUndoEdit() && !proc.canRedoEdit(), "a loaded project starts a history of its own");
    }

    std::printf("\na source's settings: what it sends, and a note learned for a trigger\n");
    {
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;

        //  LFO 1 as a sine locked to the tempo, one cycle an eighth note -- set here rather than taken from its
        //  defaults, so a later default change cannot break this. Its value at the last control step -- 256
        //  samples into the last block -- follows from the timeline alone: at 120 bpm, two quarter notes a
        //  second, and half of one a cycle.
        for (const auto& [id, value] :
             {std::pair{bambi::EncoderParam::Lfo1Sync, 1.0f}, std::pair{bambi::EncoderParam::Lfo1Div, 12.0f},
              std::pair{bambi::EncoderParam::Lfo1Shape, 0.0f}})
            if (auto* param = proc.hostParameter(id)) param->setValueNotifyingHost(param->convertTo0to1(value));
        check(bambi::formatParameter(bambi::EncoderParam::Lfo1Div, 12.0) == "1/8", "division 12 is an eighth note");
        const int run = 37;
        for (int b = 0; b < run; ++b) {
            head.time = static_cast<juce::int64>(b) * 512;
            proc.processBlock(buffer, midi);
        }
        const double lastStep = static_cast<double>((run - 1) * 512 + 256);
        const double cycles = lastStep / kRate * 2.0 / 0.5;
        const double expected = std::sin(2.0 * juce::MathConstants<double>::pi * std::fmod(cycles, 1.0));
        const double sent = proc.diagnostics().sourceValues[12].load();
        check(std::abs(sent - expected) < 1e-4, "lfo 1's output reaches the editor as the engine sent it (" +
                                                    juce::String(sent, 4) + ", expected " + juce::String(expected, 4) +
                                                    ")");

        int envelope = -1, channel = 0, note = 0;
        midi.addEvent(juce::MidiMessage::noteOn(3, 50, 1.0f), 10);
        proc.processBlock(buffer, midi);
        midi.clear();
        const bool unarmed = !proc.takeLearnedNote(envelope, channel, note);

        proc.armNoteLearn(1);
        midi.addEvent(juce::MidiMessage::noteOff(10, 40), 5);
        midi.addEvent(juce::MidiMessage::noteOn(10, 41, static_cast<juce::uint8>(0)), 50);  // velocity 0: a note-off
        midi.addEvent(juce::MidiMessage::noteOn(10, 40, 0.8f), 100);
        midi.addEvent(juce::MidiMessage::noteOn(11, 45, 0.8f), 200);  // only the first note-on counts
        proc.processBlock(buffer, midi);
        midi.clear();
        const bool caught = proc.takeLearnedNote(envelope, channel, note);
        check(unarmed && caught && envelope == 1 && channel == 10 && note == 40,
              "armed for envelope 2, the next note-on is caught with its channel; unarmed, a note is only a note");
        check(proc.noteLearnArmed() == -1 && !proc.takeLearnedNote(envelope, channel, note),
              "learning disarms itself, and a note is taken once");
    }

    std::printf("\nmidi fires each generator envelope from its own notes\n");
    {
        //  Envelope 1 routed to width at full depth, and nothing else routed.
        bambi::PluginState patch{bambi::encodeParams()};
        bambi::generatorDefaults(patch.trajectory.generator, patch.trajectory.genParams);
        patch.matrix.push_back({bambi::MatrixTab::Generators, 3, bambi::EncoderParam::RenderWidth, 1.0});
        const std::string patchText = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), patch);

        //  Hits at arbitrary samples, so they fall inside blocks and inside control hops.
        std::vector<TimedNote> kicks, snares;
        for (juce::int64 t : {4801, 30000, 61234, 90007, 150001, 199999}) {
            kicks.push_back({t, juce::MidiMessage::noteOn(10, 36, 1.0f)});
            kicks.push_back({t + 2400, juce::MidiMessage::noteOff(10, 36)});
            snares.push_back({t, juce::MidiMessage::noteOn(10, 38, 1.0f)});
            snares.push_back({t + 2400, juce::MidiMessage::noteOff(10, 38)});
        }
        const auto l = layout(stereo, stereo, Set::ambisonic(3));
        const auto silent = render(l, 512, 5.0, false, patchText);
        const auto kick = render(l, 512, 5.0, false, patchText, kicks);
        const auto snare = render(l, 512, 5.0, false, patchText, snares);
        check(peak(silent) > 0.01f && !firstDifference(silent, kick).isEmpty(),
              "C1 fires envelope 1, and the width it drives changes the output");
        const auto snareDiff = firstDifference(silent, snare);
        check(snareDiff.isEmpty(), "D1 belongs to envelope 2, which drives nothing: bit-identical to no MIDI" +
                                       (snareDiff.isEmpty() ? juce::String() : "  (" + snareDiff + ")"));
        for (int block : {64, 128, 480, 1024, 2048}) {
            const auto other = render(l, block, 5.0, false, patchText, kicks);
            const auto diff = firstDifference(kick, other);
            check(diff.isEmpty(), "with notes, block " + juce::String(block) + " renders bit-identical to block 512" +
                                      (diff.isEmpty() ? juce::String() : "  (" + diff + ")"));
        }
    }

    std::printf("\nstate reaches the audio thread without a lock\n");
    {
        const int snapshotsBefore = BambiEncoderProcessor::liveEngineSnapshots();
        const int freedBefore = BambiEncoderProcessor::engineSnapshotsFreedOnAudioThread();
        auto proc = std::make_unique<BambiEncoderProcessor>();
        PlayingHead head;
        proc->setPlayHead(&head);
        proc->setRateAndBufferSizeDetails(kRate, 256);
        proc->prepareToPlay(kRate, 256);

        //  Two different patches to alternate between.
        juce::MemoryBlock base;
        proc->getStateInformation(base);
        bambi::PluginState stateA{bambi::encodeParams()};
        bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                         std::string_view(static_cast<const char*>(base.getData()), base.getSize()), stateA);
        bambi::PluginState stateB = stateA;
        stateB.matrix.push_back({bambi::MatrixTab::Features, 0, bambi::EncoderParam::RenderWidth, 0.5});
        const std::string textA = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), stateA),
                          textB = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), stateB);

        //  Another thread holds the audio callback's lock for a second. A state load that needed
        //  it would wait that long; one that does not returns at once.
        std::atomic<bool> held{false};
        std::thread holder([&] {
            const juce::ScopedLock lock(proc->getCallbackLock());
            held.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        });
        while (!held.load()) std::this_thread::yield();
        const auto t0 = std::chrono::steady_clock::now();
        proc->setStateInformation(textB.data(), static_cast<int>(textB.size()));
        const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        holder.join();
        check(loadMs < 200.0, "a state load does not wait for the audio callback's lock (" + juce::String(loadMs, 1) +
                                  " ms, with the lock held for 1000)");

        //  An audio thread runs flat out while the message thread loads 400 states into it.
        std::atomic<bool> stop{false};
        std::atomic<long> blocks{0};
        std::thread audio([&] {
            juce::AudioBuffer<float> buffer(
                std::max(proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()), 256);
            juce::MidiBuffer midi;
            juce::int64 t = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                buffer.clear();
                for (int i = 0; i < 256; ++i)
                    buffer.setSample(0, i, 0.2f * std::sin(0.03f * static_cast<float>(t + i)));
                head.time = t;
                proc->processBlock(buffer, midi);
                t += 256;
                blocks.fetch_add(1, std::memory_order_relaxed);
            }
        });
        constexpr int kLoads = 400;
        for (int i = 0; i < kLoads; ++i) {
            const auto& text = (i % 2 == 0) ? textA : textB;
            proc->setStateInformation(text.data(), static_cast<int>(text.size()));
            if (i % 4 == 3) proc->linkTick();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        /*  Then 40 more with the message thread never calling linkTick. Posting drains first, so one
            post retires at most one snapshot and frees it again, and the retire queue never fills.
            What the 40 prove is that nothing defers and nothing leaks, although nothing is collected
            here. The guard in adopt() stays as the backstop it is. */
        constexpr int kUncollected = 40;
        for (int i = 0; i < kUncollected; ++i) {
            const auto& text = (i % 2 == 0) ? textA : textB;
            proc->setStateInformation(text.data(), static_cast<int>(text.size()));
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        stop.store(true);
        audio.join();

        juce::AudioBuffer<float> last(std::max(proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()),
                                      256);
        juce::MidiBuffer lastMidi;
        proc->linkTick();                    // frees what was retired, making room
        proc->processBlock(last, lastMidi);  // adopts whatever is still pending
        proc->linkTick();
        const auto adopted = proc->diagnostics().stateSequence.load();
        check(blocks.load() > 100 && adopted == static_cast<std::uint64_t>(kLoads + kUncollected + 1),
              juce::String(kLoads + kUncollected) + " state loads while " +
                  juce::String(static_cast<juce::int64>(blocks.load())) +
                  " blocks ran: the audio thread ends on the last one (v" +
                  juce::String(static_cast<juce::int64>(adopted)) + ")");
        check(BambiEncoderProcessor::engineSnapshotsFreedOnAudioThread() == freedBefore,
              "no snapshot was ever freed inside processBlock");
        const auto deferred = proc->diagnostics().adoptionsDeferred.load();
        check(deferred == 0 && BambiEncoderProcessor::liveEngineSnapshots() == snapshotsBefore + 1,
              "none leaked and none deferred (" + juce::String(static_cast<juce::int64>(deferred)) +
                  "), the retire queue drained by posting: " +
                  juce::String(BambiEncoderProcessor::liveEngineSnapshots() - snapshotsBefore) +
                  " snapshot alive, the current one");
        const auto session = proc->identity().session;
        proc.reset();
        check(BambiEncoderProcessor::liveEngineSnapshots() == snapshotsBefore,
              "and closing the instance frees that one too");
        if (!session.isNil()) bambi::LinkBus::unlinkSession(session);
    }

    std::printf("\nthe matrix: three rows always, and a touched parameter earns one\n");
    {
        BambiEncoderProcessor proc;
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        const auto saved = [&proc] {
            juce::MemoryBlock block;
            proc.getStateInformation(block);
            bambi::PluginState s{bambi::encodeParams()};
            bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                             std::string_view(static_cast<const char*>(block.getData()), block.getSize()), s);
            return s;
        };
        const auto rows = bambi::matrixTargets(bambi::encodeMod(), saved());
        check(shell != nullptr && rows.size() == 1 && rows[0] == bambi::EncoderParam::MotionSpeed,
              "a fresh instance already has one row, speed's");

        if (shell != nullptr) {
            //  The source tab, where speed and distance both live.
            shell->showControls(2, bambi::MatrixTab::Features);
            const bool touchedBase = shell->touchParameter(bambi::EncoderParam::MotionSpeed);
            const bool baseAddsNothing = shell->provisionalRow() == bambi::kNoParamId;
            //  the gain is a level in the column, and a click there opens its row as a tile's does
            const bool touchedOther = shell->clickLevel(bambi::EncoderParam::RenderGain);
            const bool otherOpensRow = shell->provisionalRow() == bambi::EncoderParam::RenderGain;
            const bool touchedGain = shell->touchParameter(bambi::EncoderParam::InputTrim);
            const bool replaced = shell->provisionalRow() == bambi::EncoderParam::InputTrim;
            check(touchedBase && baseAddsNothing && touchedOther && otherOpensRow && touchedGain && replaced,
                  "touching a target opens its row, the next touch replaces it, and one that has a row already opens "
                  "none");

            //  A name that takes no modulation is not a row at all.
            const bool touchedChoice = shell->touchParameter(bambi::EncoderParam::MotionDirection);
            check(!touchedChoice || shell->provisionalRow() == bambi::EncoderParam::InputTrim,
                  "touching a parameter that takes no modulation changes nothing");
        }
    }

    std::printf("\na stereo input: sum, mid/side, stereo\n");
    {
        /*  Through the processor, with a real stereo bus: what the core tests cannot reach is the
            feed -- that the second encoder is given the side, (L - R) / 2, and is run at all.
            Energy in W (channel 0) and in everything else, after the ramps have settled. */
        struct Heard {
            double w{0.0}, rest{0.0};
        };
        const auto render = [&](int mode, float left, float right, bool monoBus) {
            BambiEncoderProcessor proc;
            if (monoBus)
                proc.setBusesLayout(layout(juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo(),
                                           juce::AudioChannelSet::ambisonic(3)));
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 512);
            proc.prepareToPlay(kRate, 512);
            setParameter(proc, "input.mode", static_cast<float>(mode));
            setParameter(proc, "input.spread", 90.0f);
            setParameter(proc, "input.offset", 0.25f);
            setParameter(proc, "motion.speed", 0.0f);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            Heard heard;
            for (int block = 0; block < 12; ++block) {
                buffer.clear();
                for (int i = 0; i < 512; ++i) {
                    const float x = std::sin(0.05f * static_cast<float>(block * 512 + i));
                    buffer.setSample(0, i, x * left);
                    if (!monoBus) buffer.setSample(1, i, x * right);
                }
                proc.processBlock(buffer, midi);
                head.time += 512;
                if (block < 6) continue;  // the ramps from silence have run out by here
                for (int i = 0; i < 512; ++i) {
                    heard.w += static_cast<double>(buffer.getSample(0, i)) * buffer.getSample(0, i);
                    for (int c = 1; c < 16; ++c)
                        heard.rest += static_cast<double>(buffer.getSample(c, i)) * buffer.getSample(c, i);
                }
            }
            return heard;
        };

        /*  A pure side, L = -R. `sum` hears nothing of it, as it always has. In mid/side it is on the
            sphere and not in W, which is the whole reason the side is a difference of two points: a
            mono fold-down of the output is then the mid alone. Catches the side encoder not being
            run, being fed the mid, or being aimed at a sum. */
        const auto sumSide = render(0, 1.0f, -1.0f, false);
        const auto msSide = render(1, 1.0f, -1.0f, false);
        check(sumSide.w < 1e-9 && sumSide.rest < 1e-9, "sum hears nothing of a pure side, as it always has");
        check(msSide.rest > 1.0 && msSide.w < 1e-9 * msSide.rest,
              "mid/side puts it on the sphere and keeps it out of W: a mono fold-down is the mid alone");

        /*  Stereo tells left from right; `sum` cannot. Catches both channels landing on one point. */
        const auto sumL = render(0, 1.0f, 0.0f, false), sumR = render(0, 0.0f, 1.0f, false);
        const auto stL = render(2, 1.0f, 0.0f, false);
        check(std::abs(sumL.rest - sumR.rest) < 1e-9 * sumL.rest,
              "sum puts a left-only and a right-only input in one place");
        check(stL.rest > 0.0 && std::abs(stL.w - sumL.w) > 1e-6 * sumL.w,
              "stereo does not: a left-only input is one of two points, at two points' level");

        /*  A mono track has no side, so every mode is `sum` there -- to the bit, because the side
            encoder is fed silence and adds nothing. Catches a mono bus reading a second channel
            that is not its own. */
        const auto monoSum = render(0, 1.0f, 0.0f, true), monoMs = render(1, 1.0f, 0.0f, true);
        const auto monoSt = render(2, 1.0f, 0.0f, true);
        check(
            monoSum.w > 0.0 && juce::exactlyEqual(monoSum.w, monoMs.w) && juce::exactlyEqual(monoSum.rest, monoMs.rest),
            "on a mono track mid/side is sum, exactly");
        check(juce::exactlyEqual(monoSum.w, monoSt.w) && juce::exactlyEqual(monoSum.rest, monoSt.rest),
              "and so is stereo: a mono source is one point, not two");
    }

    std::printf("\nwhat the panel shows is what the host has\n");
    {
        /*  A host parameter lives in the host's parameters and not in the document, so controls that
            read the document alone go on drawing the value a patch was saved with: the click lands,
            the engine follows, and the panel never says so. Both catch `LocalTarget::state()`
            answering from the document without the live values. */
        BambiEncoderProcessor proc;
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell == nullptr) {
            check(false, "no editor: the shown-value checks cannot run");
        } else {
            shell->showControls(2, bambi::MatrixTab::Features);
            const auto before = shell->shownValue(bambi::EncoderParam::MotionDirection);
            const bool clicked = shell->touchParameter(bambi::EncoderParam::MotionDirection);
            shell->tick();
            check(clicked && !juce::exactlyEqual(shell->shownValue(bambi::EncoderParam::MotionDirection), before),
                  "a choice stepped by a click shows its new option on the next frame");

            const auto rate = bambi::parameterByKey("region1.yaw_rate");
            if (auto* p = proc.hostParameter(rate)) p->setValueNotifyingHost(p->convertTo0to1(45.0f));
            shell->tick();
            check(std::abs(shell->shownValue(rate) - 45.0f) < 0.01f,
                  "and a region's yaw rate set in the host is the rate its tile shows");

            /*  What is true of the whole instance is always on screen: the gain in the level
                column, `rates continue` in the footer, and the readout beside it -- on no page, so
                no tab can hide them. Pressed where they are drawn. Catches the column's drag or the
                footer's click not being wired, and the switch still living on a page instead. */
            {
                auto* gain = proc.hostParameter(bambi::EncoderParam::RenderGain);
                const float gainBefore = gain == nullptr ? -1.0f : gain->getValue();
                const bool dragged = shell->dragLevel(bambi::EncoderParam::RenderGain, -40.0f);
                check(dragged && gain != nullptr && gain->getValue() > gainBefore,
                      "the gain is in the level column, and an upward drag raises it");

                auto* rates = proc.hostParameter(bambi::EncoderParam::RatesRetrigger);
                const bool clicked = shell->clickFooterSwitch(bambi::EncoderParam::RatesRetrigger);
                check(clicked && rates != nullptr && rates->getValue() > 0.5f,
                      "rates continue is in the footer, and a click turns it on");
                shell->tick();  // a frame passes between two clicks, and the controls re-read the host
                shell->clickFooterSwitch(bambi::EncoderParam::RatesRetrigger);
                check(rates != nullptr && rates->getValue() < 0.5f, "and a second click turns it off");

                for (int tab = 0; tab < 3; ++tab) {
                    shell->showControls(tab, bambi::MatrixTab::Features);
                    check(!shell->drawnOnPage(bambi::EncoderParam::RatesRetrigger),
                          "and no page draws it a second time (tab " + juce::String(tab) + ")");
                }
                shell->showControls(2, bambi::MatrixTab::Features);
                shell->tick();
                check(shell->footerReadout().contains("order") && shell->footerReadout().contains("dsp"),
                      "the footer reads the order and this plugin's own load (" + shell->footerReadout() + ")");
            }

            /*  The input mode is at the top of the source tab, and its two values are both there
                whatever it says -- the one not in use drawn and inert. The segments are clicked at
                their middle, which is `mid/side`. Catches the choice not reaching the host, and a
                greyed tile that still takes a press. */
            {
                shell->showControls(2, bambi::MatrixTab::Features);
                auto* mode = proc.hostParameter(bambi::EncoderParam::InputMode);
                check(shell->drawnSomewhere(bambi::EncoderParam::InputSpread) &&
                          shell->drawnSomewhere(bambi::EncoderParam::InputOffset),
                      "the spread and the offset are both on the source tab");
                check(!shell->touchParameter(bambi::EncoderParam::InputSpread) &&
                          !shell->touchParameter(bambi::EncoderParam::InputOffset),
                      "and in sum neither takes a press: there is nothing for them to set");
                const bool chose = shell->touchParameter(bambi::EncoderParam::InputMode);
                shell->tick();
                check(chose && mode != nullptr && juce::roundToInt(mode->convertFrom0to1(mode->getValue())) == 1,
                      "a click on mid/side sets the input mode");
                check(shell->touchParameter(bambi::EncoderParam::InputSpread) &&
                          !shell->touchParameter(bambi::EncoderParam::InputOffset),
                      "and then the spread is live and the offset still is not");
                if (mode != nullptr) mode->setValueNotifyingHost(0.0f);
                shell->tick();
            }

            /*  The settings page is the suite's, and the encoder's window opens it as the others do.
                Catches the glyph still opening only the old readout, and the name not
                reaching this instance's identity. */
            {
                shell->openSettings(true);
                check(shell->settingsOpen(), "the settings page opens");
                shell->typeInstanceName("rhodes, left");
                check(proc.identity().label == "rhodes, left", "a name typed there is this instance's");
                shell->typeInstanceName("");
                check(proc.identity().label.empty(), "and an emptied one gives it back to the track");
                check(shell->closeSettingsByItsCross() && !shell->settingsOpen(), "the cross closes it");

                /*  The encoder's own page, the host readout, opens from a button on the settings page
                    and covers what that covers -- with the footer in front of it, as in front of the
                    settings page, because what is always on screen stays on screen. Opening the
                    settings page again puts it away. Catches each of those breaking; all three are
                    the shared window's now. */
                shell->openSettings(true);
                check(shell->clickSettingsAction(0) && shell->overlayShown() && !shell->settingsOpen(),
                      "the readout opens from its button, in place of the settings page");
                check(shell->footerIsInFrontOfOverlay(), "and the footer is in front of it");
                shell->openSettings(true);
                check(shell->settingsOpen() && !shell->overlayShown(),
                      "opening the settings page puts the readout away");
                shell->openSettings(false);
            }

            /*  Undo from the header is seen at once, not a frame later: the window re-reads the patch
                when the button is pressed. Catches it waiting for the timer -- a host that throttles
                timers would keep showing the undone edit. */
            {
                const auto before = shell->shownState().regions[0].shape.kind;
                shell->chooseRegionKind(before == bambi::RegionKind::Band ? 1 : 2);
                const auto edited = shell->shownState().regions[0].shape.kind;
                const bool clicked = shell->clickUndo();  // and no tick after it
                check(clicked && edited != before && shell->shownState().regions[0].shape.kind == before,
                      "undo from the header is in the window before the next frame");
            }

            /*  The region is sampled when it moves, not whenever the scene repaints -- which the
                encoder's does every frame, for a source that moves. Catches the kept picture being
                dropped, or its key never matching. The second half catches a key that never misses. */
            shell->chooseRegionKind(1);
            shell->showSource(bambi::kNumSources - 1);
            const auto paint = [&] {
                const juce::Image painted = shell->createComponentSnapshot(shell->getLocalBounds(), true, 1.0f);
                juce::ignoreUnused(painted);
            };
            paint();
            const int built = shell->regionBuilds();
            for (int i = 0; i < 5; ++i) {
                shell->tick();
                paint();
            }
            check(built > 0 && shell->regionBuilds() == built,
                  "a region that has not moved is drawn from what was kept, however often the scene repaints");
            if (auto* p = proc.hostParameter(bambi::parameterByKey("region1.yaw")))
                p->setValueNotifyingHost(p->convertTo0to1(30.0f));
            shell->tick();
            paint();
            check(shell->regionBuilds() == built + 1, "and one that has is sampled again, once");
        }
    }

    std::printf("\nthe energy picture, in the encoder: what leaves it, and only while somebody looks\n");
    {
        /*  The encoder shows the field it makes, as an effect shows the one it is handed: what
            position and width have become at the order the track renders at. Off when a window
            opens, gathered only while it is on, all of it the plugin's own -- so all blue, under the
            orange source. Catches the audio thread gathering with nobody looking, the output not
            being gathered at all, and -- the one that matters -- the picture pointing somewhere
            the source is not: a narrow source, not moving, must put the energy's peak on itself. */
        BambiEncoderProcessor proc;
        auto l = proc.getBusesLayout();
        l.getChannelSet(false, 0) = juce::AudioChannelSet::ambisonic(3);
        proc.setBusesLayout(l);
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 128);
        setParameter(proc, "motion.speed", 0.0f);
        setParameter(proc, "render.width", 0.0f);
        proc.prepareToPlay(kRate, 128);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        128);
        juce::MidiBuffer midi;
        const auto play = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                buffer.clear();
                for (int i = 0; i < 128; ++i)
                    buffer.setSample(0, i,
                                     0.4f * static_cast<float>(std::sin(6.2831853 * 220.0 *
                                                                        static_cast<double>(head.time + i) / kRate)));
                proc.processBlock(buffer, midi);
                head.time += 128;
            }
        };
        std::vector<float> leaving, arrived;
        play(40);
        check(!proc.takeCovariances(leaving, arrived), "with nobody looking, the audio thread gathers nothing");

        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (ed != nullptr) {
            check(!ed->energyShown() && !proc.energyWanted(), "the picture is off when a window opens");
            ed->setEnergyShown(true);
            check(proc.energyWanted(), "switching it on tells the audio thread somebody is looking");
            play(40);
            const bool landed = proc.takeCovariances(leaving, arrived);
            check(landed && leaving.size() == static_cast<std::size_t>(bambi::covarianceSize(3)) && arrived.empty(),
                  "what LEAVES is published, at the track's order, and nothing arrived: an encoder has no field coming "
                  "in");

            const auto& d = proc.diagnostics();
            const double az = d.azimuthDeg.load() * bambi::kDeg2Rad, el = d.elevationDeg.load() * bambi::kDeg2Rad;
            const bambi::Vec3 source{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
            bambi::EnergyField field;
            field.prepare(3);
            field.sample(leaving, {});
            field.step(0.05);
            int best = 0;
            for (int t = 1; t < bambi::kEnergyTexels; ++t)
                if (field.energyAt(t, true) > field.energyAt(best, true)) best = t;
            const double along = bambi::dot(bambi::EnergyField::directionOf(best), source);
            check(along > 0.98, "and its peak is where the source is (" +
                                    juce::String(std::acos(std::min(1.0, along)) * bambi::kRad2Deg, 1) + " deg off)");

            editor.reset();
            check(!proc.energyWanted(), "closing the window is nobody looking: the gathering stops");
        }
    }

    std::printf("\nthe globe's view presets, in every plugin's strip\n");
    {
        /*  The strip over a scene view is one shared piece, so every plugin's globe snaps to the same
            views. Catches the chips not being drawn, a click on one not moving the camera, and a
            preset staying lit after a drag has orbited the globe away from it. */
        BambiEncoderProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (ed != nullptr) {
            const auto sameView = [](bambi::Camera a, bambi::Camera b) {
                return juce::exactlyEqual(a.yaw, b.yaw) && juce::exactlyEqual(a.pitch, b.pitch);
            };
            const auto top = bambi::cameraFor(bambi::ViewPreset::Top);
            check(!sameView(ed->sceneCamera(), top), "the globe does not open on the top view");
            check(ed->clickViewPreset(0) && ed->viewPreset() == bambi::ViewPreset::Top &&
                      sameView(ed->sceneCamera(), top),
                  "clicking `top` in the globe's strip puts the camera there and lights the chip");
            check(ed->clickViewPreset(2) && ed->viewPreset() == bambi::ViewPreset::Side &&
                      sameView(ed->sceneCamera(), bambi::cameraFor(bambi::ViewPreset::Side)),
                  "and `side` the same");
            ed->dragScene(bambi::Vec3{0.0, 0.0, 1.0}, 60.0f, 35.0f);
            check(ed->viewPreset() == bambi::ViewPreset::Free &&
                      !sameView(ed->sceneCamera(), bambi::cameraFor(bambi::ViewPreset::Side)),
                  "a drag that orbits the globe away leaves no preset lit");
        }
    }

    std::printf("\na state load puts the source ON the new path, at once\n");
    {
        /*  `snap` is what a state load asks for and an edit does not: the encoder jumps onto the new
            path, where an edit ramps it there over a hop. It rides the snapshot to the audio thread
            and the plugin acts on it in `snapshotAdopted`. It is about the path -- a host parameter is
            smoothed either way -- so the state loaded here has the same parameters and a path turned
            120 degrees, and the direction is read off the audio: a constant input, first order, and
            atan2 of Y over X at the second sample after the step. Catches the snap doing nothing,
            which leaves that sample still where the old path had it. */
        const auto azimuthAfter = [](bool asStateLoad) {
            BambiEncoderProcessor proc;
            auto l = proc.getBusesLayout();
            l.getChannelSet(false, 0) = juce::AudioChannelSet::ambisonic(1);
            proc.setBusesLayout(l);
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 256);
            setParameter(proc, "motion.speed", 0.0f);
            proc.prepareToPlay(kRate, 256);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            256);
            juce::MidiBuffer midi;
            const auto run = [&] {
                buffer.clear();
                for (int i = 0; i < 256; ++i) buffer.setSample(0, i, 0.5f);
                proc.processBlock(buffer, midi);
                head.time += 256;
                //  ACN at first order: W Y Z X
                return std::atan2(buffer.getSample(1, 1), buffer.getSample(3, 1)) * bambi::kRad2Deg;
            };
            for (int i = 0; i < 8; ++i) run();
            const double before = run();

            auto state = proc.documentState();
            bambi::convertToCustom(state.trajectory, 8);
            state.trajectory.kind = bambi::TrajectoryKind::Custom;
            const double c = std::cos(120.0 * bambi::kDeg2Rad), sn = std::sin(120.0 * bambi::kDeg2Rad);
            const auto turn = [c, sn](bambi::Vec3 v) {
                return bambi::Vec3{c * v.x - sn * v.y, sn * v.x + c * v.y, v.z};
            };
            for (auto& node : state.trajectory.nodes) {
                node.p = turn(node.p);
                node.cin = turn(node.cin);
                node.cout = turn(node.cout);
            }
            if (asStateLoad) {
                const std::string text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), state);
                proc.setStateInformation(text.data(), static_cast<int>(text.size()));
            } else {
                const auto path = state.trajectory;
                proc.edit("turned", [path](bambi::PluginState& st) { st.trajectory = path; });
            }
            const auto from = [before](double az) {
                const double turned = std::abs(az - before);
                return turned > 180.0 ? 360.0 - turned : turned;
            };
            const double first = from(run());
            double settled = first;
            for (int i = 0; i < 200; ++i)  // a second of audio: wherever it was going, it is there
                settled = from(run());
            return std::pair<double, double>{first, settled};
        };
        const auto loaded = azimuthAfter(true);
        const auto edited = azimuthAfter(false);
        check(loaded.second > 90.0 && std::abs(loaded.first - loaded.second) < 0.5,
              "a loaded state's path is where the source is from the first samples (" + juce::String(loaded.first, 2) +
                  " of " + juce::String(loaded.second, 2) + " deg)");
        check(std::abs(edited.second - loaded.second) < 0.5 && edited.first < 0.5 * edited.second,
              "and an EDIT to the same path is travelled to, not jumped (" + juce::String(edited.first, 2) + " of " +
                  juce::String(edited.second, 2) + " deg so far)");
    }

    std::printf("\nlevel.release reaches this plugin's detector\n");
    {
        //  A burst falling to a quiet tone, above the silence gate, so the release is the whole fall.
        //  Catches this processor never handing the parameter to its detector; the detector itself
        //  is `core`'s, and the field's power is checked where there is a field to cancel (Reverb).
        const auto levelAfter = [](float releaseMs) {
            BambiEncoderProcessor proc;
            proc.setBusesLayout(layout(juce::AudioChannelSet::stereo(), juce::AudioChannelSet::disabled(),
                                       juce::AudioChannelSet::ambisonic(3)));
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 256);
            setParameter(proc, "level.release", releaseMs);
            proc.prepareToPlay(kRate, 256);
            juce::AudioBuffer<float> buffer(16, 256);
            juce::MidiBuffer midi;
            const auto loud = static_cast<juce::int64>(0.5 * kRate), end = static_cast<juce::int64>(0.75 * kRate);
            for (juce::int64 at = 0; at < end; at += 256) {
                head.time = at;
                buffer.clear();
                for (int i = 0; i < 256; ++i) {
                    const double t = static_cast<double>(at + i) / kRate;
                    buffer.setSample(
                        0, i, (at + i < loud ? 0.5f : 0.01f) * static_cast<float>(std::sin(6.2831853 * 300.0 * t)));
                }
                proc.processBlock(buffer, midi);
            }
            return static_cast<bambi::host::TargetHost&>(proc).liveSourceValue(0);
        };
        const float fast = levelAfter(30.0f), slow = levelAfter(1500.0f);
        check(fast < 0.35f && slow > 0.6f, "level.release sets how fast level falls (" + juce::String(fast, 3) +
                                               " against " + juce::String(slow, 3) + ")");
    }

    std::printf("\nthe rotation rates turn the path; input trim is heard by the detectors and the encoder\n");
    {
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        for (int i = 0; i < BambiEncoderProcessor::kLinkTickHz / 10 + 2; ++i) proc.linkTick();

        //  Each axis on its own rate: the source stays put on its path, so what moves is the placement
        //  itself. Both are set before the first block, so neither pays a smoothing ramp the other does not.
        setParameter(proc, "transform.yaw_rate", 90.0f);
        setParameter(proc, "transform.pitch_rate", 90.0f);
        setParameter(proc, "motion.speed", 0.0f);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        const int blocks = 94;
        for (int b = 0; b < blocks; ++b) {
            head.time = static_cast<juce::int64>(b) * 512;
            buffer.clear();
            proc.processBlock(buffer, midi);
        }
        proc.linkTick();
        bambi::LinkScene scene;
        proc.updateScene(scene);
        const auto* self = scene.find(proc.identity().instance);
        const double turned = self != nullptr ? self->dyn.yawRad * bambi::kRad2Deg : -999.0;
        const double expected = 90.0 * (static_cast<double>(blocks) * 512.0 / kRate);
        check(self != nullptr && std::abs(turned - expected) < 1.0,
              "yaw rate at 90 deg/s turned the path " + juce::String(turned, 2) + " deg in " +
                  juce::String(static_cast<double>(blocks) * 512.0 / kRate, 2) + " s");

        //  Pitch turns on its own axis, which is the whole of 0020: a rate is not yaw's privilege.
        const double lifted = self != nullptr ? self->dyn.pitchRad * bambi::kRad2Deg : -999.0;
        check(self != nullptr && std::abs(lifted - expected) < 1.0,
              "pitch rate at 90 deg/s tilted the path " + juce::String(lifted, 2) + " deg, on its own axis");

        //  Double-clicking yaw back to zero means yaw at zero, not the parameter at zero under the turn its rate
        //  added: that turn is cleared, and pitch's is left where it was.
        proc.zeroRotation(0);
        head.time = static_cast<juce::int64>(blocks) * 512;
        buffer.clear();
        proc.processBlock(buffer, midi);
        proc.linkTick();
        proc.updateScene(scene);
        const auto* zeroed = scene.find(proc.identity().instance);
        const double oneBlock = 90.0 * 512.0 / kRate;  // the rate keeps turning from zero through the block
        const double zeroedYaw = zeroed != nullptr ? zeroed->dyn.yawRad * bambi::kRad2Deg : -999.0;
        const double keptPitch = zeroed != nullptr ? zeroed->dyn.pitchRad * bambi::kRad2Deg : -999.0;
        check(std::abs(zeroedYaw) < oneBlock + 0.5 && std::abs(keptPitch - (expected + oneBlock)) < 1.0,
              "clearing yaw's turn brings it back to " + juce::String(zeroedYaw, 2) + " deg, and pitch keeps its " +
                  juce::String(keptPitch, 2) + " deg");

        //  The transport jumps back: an integrated rotation belongs to the take that started it.
        head.time = 0;
        buffer.clear();
        proc.processBlock(buffer, midi);
        proc.linkTick();
        proc.updateScene(scene);
        const auto* after = scene.find(proc.identity().instance);
        check(after != nullptr && std::abs(after->dyn.yawRad) * bambi::kRad2Deg < 1.5 &&
                  std::abs(after->dyn.pitchRad) * bambi::kRad2Deg < 1.5,
              "and both start again from the placement when the transport does, so a bounce repeats");

        //  Trim is applied where both the detectors and the encoder see it.
        const auto l = layout(stereo, stereo, Set::ambisonic(3));
        bambi::PluginState plain{bambi::encodeParams()};
        bambi::generatorDefaults(plain.trajectory.generator, plain.trajectory.genParams);
        bambi::PluginState trimmed = plain;
        trimmed.params[static_cast<std::size_t>(bambi::EncoderParam::InputTrim)] = -12.0f;
        const auto full =
            render(l, 512, 1.0, false, bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), plain));
        const auto quiet =
            render(l, 512, 1.0, false, bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), trimmed));
        const float ratio = peak(quiet) / std::max(peak(full), 1.0e-9f);
        check(std::abs(ratio - 0.251f) < 0.02f,
              "input trim at -12 dB renders at " + juce::String(ratio, 3) + " of full scale");

        //  The region is heard through the real processor: a region that is everywhere turns the gain
        //  down through its matrix cell, and the same region used from its outside passes nothing to it.
        //  Catches the processor never handing its region to the control step.
        bambi::PluginState inside = plain;
        inside.regions[0].shape.kind = bambi::RegionKind::Everywhere;
        inside.matrix.push_back(bambi::MatrixCell{bambi::MatrixTab::Region, 0, bambi::EncoderParam::RenderGain, -0.25});
        bambi::PluginState outside = inside;
        outside.params[static_cast<std::size_t>(bambi::EncoderParam::Region1Side)] = 1.0f;  // outside the region
        const float ducked =
            peak(render(l, 512, 1.0, false, bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), inside))) /
            std::max(peak(full), 1.0e-9f);
        const float passed = peak(render(l, 512, 1.0, false,
                                         bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), outside))) /
                             std::max(peak(full), 1.0e-9f);
        check(ducked < 0.2f && std::abs(passed - 1.0f) < 0.02f,
              "a region drives gain through its cell: inside renders at " + juce::String(ducked, 3) +
                  " of full scale, used from outside at " + juce::String(passed, 3));
    }

    std::printf("\nevery modulated tile shows where the engine has it\n");
    {
        /*  An LFO on width: the tile keeps saying what is set, and a live mark shows where the engine has it.
            Only five of the encoder's tiles had one, pieced together from what happened to be published.
            Catches the page's default live mark answering nothing. */
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell != nullptr) {
            shell->showControls(2, bambi::MatrixTab::Features);  // the source tab, where width is
            setParameter(proc, "render.width", 60.0f);
            setParameter(proc, "lfo1.sync", 0.0f);
            setParameter(proc, "lfo1.rate", 2.0f);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            const auto play = [&](int blocks) {
                for (int b = 0; b < blocks; ++b) {
                    buffer.clear();
                    proc.processBlock(buffer, midi);
                    head.time += 512;
                    proc.linkTick();
                }
            };
            play(20);
            shell->tick();
            double idle = 0.0;
            const bool before = shell->liveMark(bambi::EncoderParam::RenderWidth, idle) && std::abs(idle - 60.0) > 0.5;
            proc.document().edit("width row", [](bambi::PluginState& st) {
                st.matrix.push_back({bambi::MatrixTab::Generators, 0, bambi::EncoderParam::RenderWidth, 0.3});
            });
            bool marked = false;
            for (int i = 0; i < 20 && !marked; ++i) {
                play(4);
                shell->tick();
                double applied = 0.0;
                marked = shell->liveMark(bambi::EncoderParam::RenderWidth, applied) && std::abs(applied - 60.0) > 2.0;
            }
            check(!before, "a width nothing drives shows no live mark apart from its value");
            check(marked, "and with an LFO on it, the width tile shows where the engine has it");
        } else
            check(false, "the encoder's window is the shared one");
    }

    std::printf("\na path's settings modulate, and the scene shows the path the engine plays\n");
    {
        /*  An LFO on the orbit's aperture. The source moves as the ring breathes, and a live path
            is drawn beside the set one; without the row there is none. Catches the path's settings not reaching
            the processor's control step, and the live path drawn whether or not anything moves it. */
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell != nullptr) {
            setParameter(proc, "orbit.aperture", 40.0f);
            setParameter(proc, "lfo1.sync", 0.0f);
            setParameter(proc, "lfo1.rate", 2.0f);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            const auto play = [&](int blocks) {
                for (int b = 0; b < blocks; ++b) {
                    buffer.clear();
                    proc.processBlock(buffer, midi);
                    head.time += 512;
                    proc.linkTick();
                }
            };
            play(40);
            check(!shell->livePathDrawn(), "with nothing moving it, the path is drawn once");
            proc.document().edit("aperture row", [](bambi::PluginState& st) {
                st.matrix.push_back({bambi::MatrixTab::Generators, 0, bambi::EncoderParam::OrbitAperture, 0.3});
            });
            bambi::Vec3 lo{}, hi{};
            double spread = 0.0;
            for (int b = 0; b < 40; ++b) {
                play(1);
                const auto& d = proc.diagnostics();
                const bambi::Vec3 at =
                    bambi::fromAzEl(d.azimuthDeg.load() * bambi::kDeg2Rad, d.elevationDeg.load() * bambi::kDeg2Rad);
                if (b == 0) lo = at;
                spread = std::max(spread, bambi::arc(lo, at));
                hi = at;
            }
            check(shell->livePathDrawn(),
                  "an LFO on the orbit's aperture draws the path the engine plays beside the set one");
            check(spread > 5.0 * bambi::kDeg2Rad, "and the source moves as the ring breathes (" +
                                                      juce::String(spread * bambi::kRad2Deg, 1) + " degrees)");
            proc.document().edit("no aperture row", [](bambi::PluginState& st) { st.matrix.clear(); });
            play(120);
            check(!shell->livePathDrawn(), "and without the row, the live path goes");
        } else
            check(false, "the encoder's window is the shared one");
    }

    std::printf("\nthe placement's angles show where the engine has them, not only where they were set\n");
    {
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell != nullptr) {
            shell->showControls(1, bambi::MatrixTab::Features);  // the transform tab
            for (int i = 0; i < BambiEncoderProcessor::kLinkTickHz / 10 + 2; ++i) proc.linkTick();
            shell->tick();

            /*  Nothing is turning yet. The angle reads its parameter, and nothing is marked -- which is the
                half that keeps this honest: a mark that is always on would say nothing. */
            double idle = -999.0;
            const bool idleMarked = shell->liveMark(bambi::EncoderParam::TransformYaw, idle);

            setParameter(proc, "transform.yaw_rate", 90.0f);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            for (int b = 0; b < 47; ++b) {
                head.time = static_cast<juce::int64>(b) * 512;
                buffer.clear();
                proc.processBlock(buffer, midi);
            }
            proc.linkTick();
            shell->tick();

            double applied = -999.0;
            const bool marked = shell->liveMark(bambi::EncoderParam::TransformYaw, applied);
            bambi::LinkScene sc;
            proc.updateScene(sc);
            const auto* self = sc.find(proc.identity().instance);
            const double onBus = self != nullptr ? self->dyn.yawRad * bambi::kRad2Deg : -999.0;

            check(!idleMarked && std::abs(idle) < 0.01,
                  "an angle no rate is turning reads its parameter and is not marked");
            //  Against the bus rather than a predicted angle: what is under test is that the tab shows what
            //  the engine published, not how precisely the clock integrates -- which is checked above.
            check(marked && onBus > 30.0 && std::abs(applied - onBus) < 0.01,
                  "a turning yaw is marked where the engine has it (" + juce::String(applied, 2) + " deg, set to 0)");

            /*  What is registered for repaint has to be what was drawn. The mark straddles the bar, and the
                bar sits flush with the tile's bottom edge, so a tile-sized region leaves the mark's foot
                unrepainted: it stands still while the rest of it moves, until something forces a full
                repaint. Only yaw turns here, so the region is exactly one mark. */
            namespace ctl = bambi::ui::theme::controls;
            const auto region = shell->liveMarkBounds();
            const float want = ctl::barHeight + 2.0f * ctl::liveMarkOver;
            check(std::abs(region.getHeight() - want) < 0.51f,
                  "the repaint region is the mark's own extent, not the tile around it (" +
                      juce::String(region.getHeight(), 1) + " px, want " + juce::String(want, 1) + ")");
        }
    }

    std::printf("\nthe sidechain: its six sources carry the sidechain's audio, not the main input's\n");
    {
        /*  The slots are 0-5 self, 6-11 sidechain (modulation.hpp). This measures the values
            themselves, with different audio on each bus so the two cannot be confused. */
        BambiEncoderProcessor proc;
        if (proc.setBusesLayout(layout(stereo, stereo, Set::ambisonic(3)))) {
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 512);
            proc.prepareToPlay(kRate, 512);
            const int mainCh = proc.getChannelCountOfBus(true, 0);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;

            //  Main: silence. Sidechain: a loud tone. If the sidechain six read zero, its audio never
            //  reached the detectors; if they mirror slots 0-5, the two buses have been confused.
            for (int b = 0; b < 200; ++b) {
                buffer.clear();
                for (int c = 0; c < 2; ++c) {
                    auto* x = buffer.getWritePointer(mainCh + c);
                    for (int i = 0; i < 512; ++i)
                        x[i] = 0.6f * std::sin(6.2831853f * 220.0f * static_cast<float>(b * 512 + i) / kRate);
                }
                head.time = static_cast<juce::int64>(b) * 512;
                proc.processBlock(buffer, midi);
            }

            const auto& d = proc.diagnostics();
            juce::String selfSlots, sideSlots;
            for (int i = 0; i < bambi::kSourcesPerTab; ++i) {
                selfSlots += " " + juce::String(d.sourceValues[static_cast<std::size_t>(i)].load(), 3);
                sideSlots +=
                    " " + juce::String(d.sourceValues[static_cast<std::size_t>(i + bambi::kSourcesPerTab)].load(), 3);
            }
            std::printf("    self 0-5     %s\n    sidechain 6-11%s\n", selfSlots.toRawUTF8(), sideSlots.toRawUTF8());

            double sideSum = 0.0, selfSum = 0.0;
            for (int i = 0; i < bambi::kSourcesPerTab; ++i) {
                selfSum += std::abs(d.sourceValues[static_cast<std::size_t>(i)].load());
                sideSum += std::abs(d.sourceValues[static_cast<std::size_t>(i + bambi::kSourcesPerTab)].load());
            }
            check(sideSum > 0.01,
                  "a tone on the sidechain moves its six sources (sum " + juce::String(sideSum, 3) + ")");
            check(selfSum < 0.01,
                  "and silence on the main input leaves its six at rest (sum " + juce::String(selfSum, 3) + ")");
        }

        /*  The same audio on both buses -- one loop into the input and the same loop into the sidechain,
            which is how this gets set up in practice. The two banks are the same code on the same signal,
            so the six self sources and the six sidechain sources must agree. If they do not, "the sidechain
            values look off" is a defect in the sidechain bank rather than in anything a host did. */
        BambiEncoderProcessor both;
        if (both.setBusesLayout(layout(stereo, stereo, Set::ambisonic(3)))) {
            PlayingHead head;
            both.setPlayHead(&head);
            both.setRateAndBufferSizeDetails(kRate, 512);
            both.prepareToPlay(kRate, 512);
            const int mainCh = both.getChannelCountOfBus(true, 0);
            juce::AudioBuffer<float> buffer(std::max(both.getTotalNumInputChannels(), both.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;

            //  Broadband and deterministic, so every detector has something to read and none of them rails
            //  the way a pure sine makes tonal, low and mid do.
            std::uint32_t seed = 22222u;
            for (int b = 0; b < 200; ++b) {
                buffer.clear();
                for (int i = 0; i < 512; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    const float v = 0.4f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
                    for (int c = 0; c < 2; ++c) {
                        buffer.setSample(c, i, v);           // main input
                        buffer.setSample(mainCh + c, i, v);  // sidechain: the very same samples
                    }
                }
                head.time = static_cast<juce::int64>(b) * 512;
                both.processBlock(buffer, midi);
            }

            const auto& d = both.diagnostics();
            juce::String shown;
            double worst = 0.0;
            int worstAt = -1;
            for (int i = 0; i < bambi::kSourcesPerTab; ++i) {
                const double a = d.sourceValues[static_cast<std::size_t>(i)].load();
                const double b2 = d.sourceValues[static_cast<std::size_t>(i + bambi::kSourcesPerTab)].load();
                shown +=
                    "  " +
                    juce::String(bambi::sourceColumn(bambi::encodeMod(), bambi::MatrixTab::Features, i).name.data()) +
                    " " + juce::String(a, 3) + "/" + juce::String(b2, 3);
                if (std::abs(a - b2) > worst) {
                    worst = std::abs(a - b2);
                    worstAt = i;
                }
            }
            std::printf("    self/sidechain:%s\n", shown.toRawUTF8());
            check(worst < 0.02,
                  "the same audio on both buses reads the same on both sets of sources (worst " +
                      juce::String(worst, 4) +
                      (worstAt >= 0 ? " on " + juce::String(bambi::sourceColumn(bambi::encodeMod(),
                                                                                bambi::MatrixTab::Features, worstAt)
                                                                .name.data())
                                    : juce::String()) +
                      ")");
        }
    }

    std::printf("\nthe sidechain: audio on that bus reaches the sidechain detectors, at any width\n");
    {
        /*  Hosts negotiate different sidechain widths -- VST3 0, AU 16, CLAP 2 -- so this checks whether
            the width itself breaks the reading, or only the layout accepted at a given width. */
        const auto feed = [](juce::AudioProcessor::WrapperType as, const Layout& l) {
            const AsFormat format(as);
            BambiEncoderProcessor proc;
            if (!proc.setBusesLayout(l)) return -1.0f;
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 512);
            proc.prepareToPlay(kRate, 512);

            const int mainCh = proc.getChannelCountOfBus(true, 0);
            const int sideCh = proc.getBusCount(true) > 1 ? proc.getChannelCountOfBus(true, 1) : 0;
            const int total = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            juce::AudioBuffer<float> buffer(std::max(1, total), 512);
            juce::MidiBuffer midi;

            for (int b = 0; b < 8; ++b) {
                buffer.clear();
                //  Silence on the main input; a tone on the first pair of the sidechain bus, which is
                //  where the source is read from whatever the bus's width.
                for (int c = 0; c < std::min(2, sideCh); ++c) {
                    auto* x = buffer.getWritePointer(mainCh + c);
                    for (int i = 0; i < 512; ++i)
                        x[i] = 0.5f * std::sin(6.2831853f * 440.0f * static_cast<float>(b * 512 + i) / kRate);
                }
                head.time = static_cast<juce::int64>(b) * 512;
                proc.processBlock(buffer, midi);
            }
            return proc.diagnostics().sidechainPeak.load();
        };

        const float narrow = feed(vst3, layout(stereo, stereo, Set::ambisonic(3)));
        check(narrow > 0.2f,
              "a 2-channel sidechain is heard (peak " + juce::String(narrow, 3) + ") -- as VST3 and CLAP negotiate it");

        /*  A sidechain of any width, against the stereo main input every format now negotiates: the
            sidechain is read from its own bus's first pair whatever that bus's width, and that is
            checked in both formats rather than one. */
        const float wideSide = feed(vst3, layout(stereo, Set::ambisonic(3), Set::ambisonic(3)));
        check(wideSide > 0.2f,
              "a 16-channel sidechain is heard, from its first pair (peak " + juce::String(wideSide, 3) + ")");
        const float clapSide = feed(clap, layout(stereo, stereo, Set::ambisonic(3)));
        check(clapSide > 0.2f, "and as CLAP (peak " + juce::String(clapSide, 3) + ")");

        /*  At every order, not only at 3. The sidechain is read before the encode and so should not
            depend on the output width at all. */
        for (const int order : {1, 3, 5, 7}) {
            const float atOrder = feed(vst3, layout(stereo, stereo, Set::ambisonic(order)));
            check(atOrder > 0.2f, "heard with an order-" + juce::String(order) + " output (" +
                                      juce::String(bambi::numChannels(order)) + " ch, peak " +
                                      juce::String(atOrder, 3) + ")");
        }

        /*  The session people actually build: a 16-channel track so the ambisonic output fits, and a stereo
            send into track channels 3/4 -- the way every REAPER sidechain is routed. Accepting the 16-channel
            main input REAPER asks for put those channels in main pins 3 and 4, which sumToMono ignores by
            design, and the sidechain bus at pin 17 received nothing. As VST3 the request is refused and the
            wrapper settles on a stereo main input, so track channels 3/4 are the sidechain's first pair. */
        const AsFormat format(vst3);
        BambiEncoderProcessor onWideTrack;
        const auto settled = settleLikeVst3(onWideTrack, layout(Set::ambisonic(3), stereo, Set::ambisonic(3)));
        check(onWideTrack.setBusesLayout(settled),
              "a 16-channel track as VST3 settles on a layout it accepts: " + describe(settled));
        {
            PlayingHead head;
            onWideTrack.setPlayHead(&head);
            onWideTrack.setRateAndBufferSizeDetails(kRate, 512);
            onWideTrack.prepareToPlay(kRate, 512);
            juce::AudioBuffer<float> buffer(
                std::max(onWideTrack.getTotalNumInputChannels(), onWideTrack.getTotalNumOutputChannels()), 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < 200; ++b) {
                buffer.clear();
                for (int c = 2; c < 4; ++c)  // track channels 3 and 4, as the send lands
                {
                    auto* x = buffer.getWritePointer(c);
                    for (int i = 0; i < 512; ++i)
                        x[i] = 0.6f * std::sin(6.2831853f * 220.0f * static_cast<float>(b * 512 + i) / kRate);
                }
                head.time = static_cast<juce::int64>(b) * 512;
                onWideTrack.processBlock(buffer, midi);
            }
            double side = 0.0;
            for (int i = 0; i < bambi::kSourcesPerTab; ++i)
                side += std::abs(
                    onWideTrack.diagnostics().sourceValues[static_cast<std::size_t>(i + bambi::kSourcesPerTab)].load());
            check(side > 0.01, "a send into track channels 3/4 reaches the sidechain on a 16-channel track (sum " +
                                   juce::String(side, 3) + ")");
        }

        //  And with no sidechain bus at all, nothing is claimed to arrive.
        const float off = feed(vst3, layout(stereo, Set::disabled(), Set::ambisonic(3)));
        check(juce::exactlyEqual(off, 0.0f),
              "with the bus disabled nothing is heard (peak " + juce::String(off, 3) + ") -- as in VST3");
    }

    std::printf("\na timeline that cannot be exact does not reset the engine every block\n");
    {
        /*  CLAP's transport carries no sample position -- only seconds and beats -- so the wrapper derives
            timeInSamples from seconds, and it splits each host buffer into sub-blocks at events while
            reporting the host block's transport for every one of them. Demanding that the timeline advance
            by exactly numSamples therefore fires resetEngine() continuously, wiping every feature detector
            several times a block. That is what "all the features jump around" looks like. */
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;

        //  Settle: the first block after a start legitimately resets.
        head.time = 0;
        buffer.clear();
        proc.processBlock(buffer, midi);
        const int settled = proc.diagnostics().resets.load();

        //  One host block of 512, delivered as four sub-blocks that each report the host block's timeline.
        for (int i = 0; i < 4; ++i) {
            buffer.clear();
            proc.processBlock(buffer, midi);  // head.time deliberately unchanged
        }
        const int afterSubBlocks = proc.diagnostics().resets.load();
        check(afterSubBlocks == settled,
              "sub-blocks reporting the host block's timeline are continuous, not a locate (" +
                  juce::String(afterSubBlocks - settled) + " resets)");

        //  Whole blocks, with the timeline landing a sample either side of exact: seconds x rate, truncated.
        /*  Continue from where the sub-blocks left off rather than leaping to an arbitrary sample: jumping
            4096 ahead is itself a locate, and the gate is right to reset on it. */
        juce::int64 t = head.time;
        juce::String where;
        int before = proc.diagnostics().resets.load();
        for (int b = 0; b < 16; ++b) {
            const int was = proc.diagnostics().resets.load();
            t += 512 + ((b % 3) == 0 ? -1 : (b % 3) == 1 ? 1 : 0);
            head.time = t;
            buffer.clear();
            proc.processBlock(buffer, midi);
            if (proc.diagnostics().resets.load() != was) where += " b" + juce::String(b);
        }
        const int afterJitter = proc.diagnostics().resets.load();
        check(afterJitter == before, "and a timeline a sample off exact is not a locate either (" +
                                         juce::String(afterJitter - before) + " resets" + where + ")");

        //  What must still reset: a real locate. Otherwise the tolerance has swallowed the thing it is for.
        before = proc.diagnostics().resets.load();
        head.time = 0;
        buffer.clear();
        proc.processBlock(buffer, midi);
        check(proc.diagnostics().resets.load() > before, "but jumping back to the start still does");
    }

    std::printf("\nan lfo's rate means the same position to the host and to the editor\n");
    {
        //  The host parameter skews its range with JUCE; the editor and a remote edit use bambi::toNormalised.
        //  If the two drift apart, a drag in the editor lands on a different rate than the host records.
        BambiEncoderProcessor proc;
        const auto* rate = proc.hostParameter(bambi::EncoderParam::Lfo1Rate);
        float worst = rate != nullptr ? 0.0f : 1.0f;
        for (int i = 0; rate != nullptr && i <= 40; ++i) {
            const float v = 0.01f + (2.0f - 0.01f) * static_cast<float>(i) / 40.0f;
            worst = std::max(worst,
                             std::abs(rate->convertTo0to1(v) - bambi::toNormalised(bambi::EncoderParam::Lfo1Rate, v)));
        }
        check(worst < 1e-4f, "0.01 to 2 hz agree within " + juce::String(worst, 6) + ", and 0.25 hz is at " +
                                 juce::String(rate != nullptr ? rate->convertTo0to1(0.25f) : -1.0f, 3));
    }

    std::printf("\nrestart or continue: what a locate does to an lfo and to the motion\n");
    {
        /*  A locate restarts the engine (above). What is set to continue must come through it: the lfo's value and
            the source's place go on as if nothing happened, while what is set to restart begins again as a fresh
            start does. Every run is compared with an uninterrupted one AND a fresh start, and those two must
            differ, so a check that could not tell continuing from restarting fails rather than passes. */
        struct Reading {
            float lfo;
            bambi::Vec3 place;
        };
        const auto run = [](bool lfoContinues, bool motionContinues, bool locate, int blocks) {
            BambiEncoderProcessor proc;
            //  A path worth travelling: an orbit's generator parameters describe a path several degrees
            //  across, wide enough that a source moving on it is clearly different from one standing still.
            bambi::PluginState state{bambi::encodeParams()};
            state.trajectory.generator = bambi::GeneratorType::Orbit;
            bambi::generatorDefaults(bambi::GeneratorType::Orbit, state.trajectory.genParams);
            const auto text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), state);
            proc.setStateInformation(text.data(), static_cast<int>(text.size()));
            const auto set = [&proc](bambi::ParamId id, float value) {
                if (auto* param = proc.hostParameter(id)) param->setValueNotifyingHost(param->convertTo0to1(value));
            };
            set(bambi::EncoderParam::MotionSpeed, 90.0f);
            set(bambi::EncoderParam::Lfo1Sync, 0.0f);
            set(bambi::EncoderParam::Lfo1Rate, 0.5f);
            set(bambi::EncoderParam::Lfo1Retrigger, lfoContinues ? 1.0f : 0.0f);
            set(bambi::EncoderParam::RatesRetrigger, motionContinues ? 1.0f : 0.0f);
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 512);
            proc.prepareToPlay(kRate, 512);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            for (int b = 0; b <= blocks; ++b) {
                buffer.clear();
                //  the last block either plays on, or is the loop jumping back to the start
                head.time = (b == blocks && locate) ? 0 : static_cast<juce::int64>(b) * 512;
                proc.processBlock(buffer, midi);
            }
            //  The source's direction, not its azimuth alone: a path can pass its starting azimuth again at
            //  another elevation, which azimuth alone cannot tell apart.
            const auto& d = proc.diagnostics();
            return Reading{d.sourceValues[12].load(),
                           bambi::fromAzEl(static_cast<double>(d.azimuthDeg.load()) * bambi::kDeg2Rad,
                                           static_cast<double>(d.elevationDeg.load()) * bambi::kDeg2Rad)};
        };
        const auto degrees = [](bambi::Vec3 a, bambi::Vec3 b) { return bambi::arc(a, b) * bambi::kRad2Deg; };
        //  x is ten times nearer `to` than `from`
        const auto lfoNearer = [](Reading x, Reading to, Reading from) {
            return std::abs(x.lfo - to.lfo) * 10.0f < std::abs(x.lfo - from.lfo);
        };
        const auto placeNearer = [&degrees](Reading x, Reading to, Reading from) {
            return degrees(x.place, to.place) * 10.0 < degrees(x.place, from.place);
        };
        const auto describe = [](Reading r) {
            return "lfo " + juce::String(r.lfo, 3) + ", az " +
                   juce::String(bambi::azimuth(r.place) * bambi::kRad2Deg, 1) + " el " +
                   juce::String(bambi::elevation(r.place) * bambi::kRad2Deg, 1);
        };

        constexpr int kBlocks = 250;  // 2.7 s: the lfo a third of the way into its second cycle
        const auto fresh = run(false, false, false, 0);
        const auto through = run(false, false, false, kBlocks);
        check(std::abs(through.lfo - fresh.lfo) > 0.2f && degrees(through.place, fresh.place) > 10.0,
              "played through and started fresh differ (" + describe(through) + " against " + describe(fresh) + ", " +
                  juce::String(degrees(through.place, fresh.place), 1) + " degrees apart)");

        const auto restarted = run(false, false, true, kBlocks);
        check(lfoNearer(restarted, fresh, through) && placeNearer(restarted, fresh, through),
              "set to restart, a locate starts the lfo and the motion again (" + describe(restarted) + ")");
        const auto continued = run(true, true, true, kBlocks);
        check(lfoNearer(continued, through, fresh) && placeNearer(continued, through, fresh),
              "set to continue, both run on through it (" + describe(continued) + ")");
        const auto lfoOnly = run(true, false, true, kBlocks);
        check(lfoNearer(lfoOnly, through, fresh) && placeNearer(lfoOnly, fresh, through),
              "each switch is its own: the lfo continues while the motion restarts (" + describe(lfoOnly) + ")");
    }

    std::printf("\nstopped: set to restart the lfo and the motion hold, set to continue they run\n");
    {
        //  Restart links an lfo and the motion to playback, so while the transport is stopped nothing moves;
        //  continue runs all the time. Read twice while stopped, a second apart.
        const auto readings = [](bool lfoContinues, bool motionContinues) {
            BambiEncoderProcessor proc;
            bambi::PluginState state{bambi::encodeParams()};
            state.trajectory.generator = bambi::GeneratorType::Orbit;
            bambi::generatorDefaults(bambi::GeneratorType::Orbit, state.trajectory.genParams);
            const auto text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), state);
            proc.setStateInformation(text.data(), static_cast<int>(text.size()));
            for (const auto& [id, value] :
                 {std::pair{bambi::EncoderParam::MotionSpeed, 90.0f}, std::pair{bambi::EncoderParam::Lfo1Sync, 0.0f},
                  std::pair{bambi::EncoderParam::Lfo1Rate, 0.5f},
                  std::pair{bambi::EncoderParam::Lfo1Retrigger, lfoContinues ? 1.0f : 0.0f},
                  std::pair{bambi::EncoderParam::RatesRetrigger, motionContinues ? 1.0f : 0.0f}})
                if (auto* param = proc.hostParameter(id)) param->setValueNotifyingHost(param->convertTo0to1(value));
            PlayingHead head;
            head.playing = false;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 512);
            proc.prepareToPlay(kRate, 512);
            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            const auto blocks = [&](int n) {
                for (int b = 0; b < n; ++b) {
                    buffer.clear();
                    proc.processBlock(buffer, midi);
                }
                const auto& d = proc.diagnostics();
                return std::pair{d.sourceValues[12].load(),
                                 bambi::fromAzEl(static_cast<double>(d.azimuthDeg.load()) * bambi::kDeg2Rad,
                                                 static_cast<double>(d.elevationDeg.load()) * bambi::kDeg2Rad)};
            };
            const auto first = blocks(10);
            const auto second = blocks(94);  // about a second later
            return std::pair{std::abs(second.first - first.first),
                             bambi::arc(first.second, second.second) * bambi::kRad2Deg};
        };
        const auto restarting = readings(false, false);
        check(restarting.first < 1e-6f && restarting.second < 1e-6,
              "set to restart, neither moves (lfo by " + juce::String(restarting.first, 4) + ", source by " +
                  juce::String(restarting.second, 2) + " degrees)");
        const auto continuing = readings(true, true);
        check(continuing.first > 0.1f && continuing.second > 10.0,
              "set to continue, both do (lfo by " + juce::String(continuing.first, 4) + ", source by " +
                  juce::String(continuing.second, 2) + " degrees)");
        const auto lfoOnly = readings(true, false);
        check(lfoOnly.first > 0.1f && lfoOnly.second < 1e-6,
              "and each switch is its own: the lfo runs while the source holds");
    }

    std::printf("\nthe matrix scrolls under its column heads, rather than losing rows off the bottom\n");
    {
        {
            //  What a project opens on. It has to fit: a matrix that scrolls before anything is routed says
            //  the window is too short, and that is what the slack in maxScroll() was hiding.
            BambiEncoderProcessor fresh;
            std::unique_ptr<juce::AudioProcessorEditor> plain(fresh.createEditor());
            if (auto* first = dynamic_cast<BambiEncoderEditor*>(plain.get())) {
                first->tick();
                first->paintNow();
                check(juce::exactlyEqual(first->matrixScrollRange(), 0.0f),
                      "the default three rows fit without scrolling (" + juce::String(first->matrixScrollRange(), 1) +
                          " px of travel)");
            }
        }

        {
            //  One target routed: the state a project reaches the moment anyone uses the matrix.
            bambi::PluginState one{bambi::encodeParams()};
            bambi::generatorDefaults(one.trajectory.generator, one.trajectory.genParams);
            one.matrix.push_back({bambi::MatrixTab::Features, 0, bambi::EncoderParam::TransformYaw, 0.5});
            const std::string text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), one);
            BambiEncoderProcessor routed;
            routed.setStateInformation(text.data(), static_cast<int>(text.size()));
            std::unique_ptr<juce::AudioProcessorEditor> ed(routed.createEditor());
            if (auto* four = dynamic_cast<BambiEncoderEditor*>(ed.get())) {
                four->tick();
                four->paintNow();
                check(juce::exactlyEqual(four->matrixScrollRange(), 0.0f),
                      "and so does a fourth row, the moment anything is routed (" +
                          juce::String(four->matrixScrollRange(), 1) + " px of travel)");
            }
        }

        /*  A patch with most of the targets routed. Three base rows barely outgrow the viewport, so a check
            on those alone would pass while reporting ten pixels of travel and prove nothing about the case
            that matters -- a matrix taller than its window. */
        bambi::PluginState many{bambi::encodeParams()};
        bambi::generatorDefaults(many.trajectory.generator, many.trajectory.genParams);
        int column = 0;
        for (const auto target :
             {bambi::EncoderParam::MotionSpeed, bambi::EncoderParam::MotionDisplace, bambi::EncoderParam::RenderWidth,
              bambi::EncoderParam::TransformYaw, bambi::EncoderParam::TransformPitch,
              bambi::EncoderParam::TransformRoll, bambi::EncoderParam::TransformExtent,
              bambi::EncoderParam::TransformYawRate, bambi::EncoderParam::RenderGain, bambi::EncoderParam::InputTrim})
            many.matrix.push_back({bambi::MatrixTab::Features, column++ % bambi::kSourcesPerTab, target, 0.5});
        const std::string patch = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), many);

        BambiEncoderProcessor proc;
        proc.setStateInformation(patch.data(), static_cast<int>(patch.size()));
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell != nullptr) {
            shell->tick();
            shell->paintNow();

            //  Clipped-and-unreachable looks exactly like clipped-but-scrollable in a screenshot, so what
            //  gets asserted is the travel: ten routed targets are far taller than the rows area.
            const float range = shell->matrixScrollRange();
            check(range > 100.0f, "ten routed targets outgrow the window, and it says how far they reach (" +
                                      juce::String(range, 1) + " px)");

            shell->scrollMatrixTo(range * 0.5f);
            shell->paintNow();
            const bool moved = std::abs(shell->matrixScrollPosition() - range * 0.5f) < 0.51f;

            //  Past the end it clamps: a panel that scrolled into blank space would have lost its content.
            shell->scrollMatrixTo(range + 500.0f);
            shell->paintNow();
            const bool clamped = std::abs(shell->matrixScrollPosition() - range) < 0.51f;

            /*  What the viewport hides takes no press. A region is registered where it is drawn and
                later regions lie on top, so a row scrolled up under the pinned column heads can take
                the click meant for the head drawn over it -- at some scroll offsets and not others.
                Every offset, a head clicked at each. Catches the content's regions not being clipped
                to the viewport. */
            int missed = 0;
            for (float at = 0.0f; at <= range; at += 2.0f) {
                shell->showControls(0, bambi::MatrixTab::Features);  // whatever the last click opened, closed
                shell->scrollMatrixTo(at);
                shell->clickSourceColumn(bambi::MatrixTab::Features, 0);
                if (shell->shownTab() != 3 || shell->shownSource() != bambi::sourceSlot(bambi::MatrixTab::Features, 0))
                    ++missed;
            }
            check(missed == 0, "a column head answers its click at every scroll offset of the rows under it (" +
                                   juce::String(missed) + " missed)");
            shell->showControls(0, bambi::MatrixTab::Features);
            check(moved && clamped, "it scrolls where it is told, and stops at the end rather than past it");

            shell->scrollMatrixTo(0.0f);
            shell->paintNow();
            check(juce::exactlyEqual(shell->matrixScrollPosition(), 0.0f) &&
                      std::abs(shell->matrixScrollRange() - range) < 0.51f,
                  "and back at the top it reaches exactly as far as before");

            //  The right panel too: an envelope's settings is one of the two tabs taller than the window.
            shell->showSource(bambi::sourceSlot(bambi::MatrixTab::Generators, 5));  // env 3
            shell->paintNow();
            const float tall = shell->tabsScrollRange();
            check(tall > 0.0f,
                  "a tab taller than the panel reports its travel as well (" + juce::String(tall, 1) + " px)");

            shell->showControls(1, bambi::MatrixTab::Features);  // transform: short enough to fit
            shell->paintNow();
            check(juce::exactlyEqual(shell->tabsScrollRange(), 0.0f),
                  "and one that fits reports none, so the wheel does nothing there");

            /*  Scrolled to the end of a tall page, then the content shrinks. A panel that no longer overflows takes
                no wheel, so a position left past the new end could never be scrolled back: it has to come back by
                itself, or everything above it is out of reach for good. */
            shell->showSource(bambi::sourceSlot(bambi::MatrixTab::Generators, 5));
            shell->paintNow();
            shell->scrollTabsTo(shell->tabsScrollRange());
            shell->paintNow();
            const float down = shell->tabsScrollPosition();
            shell->showControls(1, bambi::MatrixTab::Features);
            shell->paintNow();
            check(down > 0.0f && juce::exactlyEqual(shell->tabsScrollPosition(), 0.0f),
                  "scrolled " + juce::String(down, 1) + " px down a tall page, a short one brings the position back (" +
                      juce::String(shell->tabsScrollPosition(), 1) + " px)");
        }
    }

    std::printf("\nthe header follows the selection, because nothing else repaints it\n");
    {
        const int joinTicks = BambiEncoderProcessor::kLinkTickHz / 10 + 2;
        auto first = std::make_unique<BambiEncoderProcessor>();
        auto second = std::make_unique<BambiEncoderProcessor>();
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }
        std::unique_ptr<juce::AudioProcessorEditor> editor(first->createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        const bool bothOnBus = first->linkStatus().peers == 2;
        if (shell != nullptr) {
            shell->tick();
            shell->paintNow();  // the header records the selection, names and count it drew

            /*  A frame in which nothing moved must not repaint it -- otherwise "it repaints" would be true
                of every frame and would prove nothing about following the selection. */
            const int settled = shell->headerRepaintsAsked();
            shell->tick();
            const bool quiet = shell->headerRepaintsAsked() == settled;

            //  The selection moves. Every panel below switches with it; the header has to as well.
            shell->selectInstance(second->identity().instance);
            const bool asked = shell->headerRepaintsAsked() > settled;

            shell->paintNow();
            const int redrawn = shell->headerRepaintsAsked();
            shell->tick();
            const bool stops = shell->headerRepaintsAsked() == redrawn;

            check(bothOnBus, "two instances share the bus, so there is another one to select");
            check(quiet, "a frame with nothing changed leaves the header alone");
            check(asked, "selecting another instance makes the header ask for a redraw");
            check(stops, "and once it has been painted it stops asking");
        }
    }

    std::printf("\nthe timing trace records the chain the scene hangs on\n");
    {
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        for (int i = 0; i < BambiEncoderProcessor::kLinkTickHz / 10 + 2; ++i) proc.linkTick();

        bambi::LinkScene scene;
        bambi::ui::FrameTrace trace(proc);
        trace.start(0.2);
        const bool started = trace.recording();
        for (int frame = 0; frame < 20 && trace.recording(); ++frame) {
            for (int b = 0; b < 3; ++b) {
                head.time = static_cast<juce::int64>(frame * 3 + b) * 512;
                proc.processBlock(buffer, midi);
            }
            proc.linkTick();
            proc.updateScene(scene);
            trace.frame(scene);
            trace.painted();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto status = trace.status();
        const juce::File file(
            status.fromFirstOccurrenceOf("wrote ", false, false).upToFirstOccurrenceOf(".csv", true, false));
        const auto text = file.loadFileAsString();
        const bool everyKind = text.contains("\nframe,") && text.contains("\npeer,") && text.contains("\nblock,") &&
                               text.contains("\ntick,") && text.contains("\npaint,");
        check(started && !trace.recording() && file.existsAsFile() && everyKind,
              "a trace ends on time and writes frames, paints, blocks, ticks and positions as csv" +
                  (file.existsAsFile() ? " (" + juce::String(text.length() / 1024) + " kB)"
                                       : juce::String(": ") + status));
        file.deleteFile();
    }

    std::printf("\nlink bus: instances find each other, and edit each other\n");
    {
        const auto tick = [](std::initializer_list<BambiEncoderProcessor*> processors, int times) {
            for (int i = 0; i < times; ++i)
                for (auto* proc : processors) proc->linkTick();
        };
        const auto shortId = [](const bambi::Uuid& u) { return juce::String(u.toString()).substring(0, 8); };
        const int joinTicks = BambiEncoderProcessor::kLinkTickHz / 10 + 2;

        auto first = std::make_unique<BambiEncoderProcessor>();
        tick({first.get()}, joinTicks);
        const auto a = first->identity();
        check(first->linkStatus().open && !a.session.isNil() && !a.instance.isNil(),
              "a new instance, with no session live, joins a fresh one (" + shortId(a.session) + ")");

        auto second = std::make_unique<BambiEncoderProcessor>();
        tick({first.get(), second.get()}, joinTicks);
        const auto b = second->identity();
        check(second->linkStatus().open && b.session == a.session && !(b.instance == a.instance),
              "a second new instance joins that session, as a different instance");
        check(first->linkStatus().peers == 2 && second->linkStatus().peers == 2, "each sees both");

        juce::MemoryBlock saved;
        first->getStateInformation(saved);
        auto copy = std::make_unique<BambiEncoderProcessor>();
        copy->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        tick({first.get(), second.get(), copy.get()}, joinTicks);
        const auto c = copy->identity();
        check(copy->linkStatus().open && c.session == a.session && !(c.instance == a.instance) &&
                  first->identity().instance == a.instance,
              "a duplicated track keeps the session and takes a new id; the original keeps its own");
        juce::MemoryBlock copySaved;
        copy->getStateInformation(copySaved);
        bambi::PluginState reread{bambi::encodeParams()};
        const bool parsed =
            bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                             std::string_view(static_cast<const char*>(copySaved.getData()), copySaved.getSize()),
                             reread)
                .ok;
        check(parsed && reread.identity.instance == c.instance,
              "the duplicate saves its new id, so the project stays de-duplicated");

        second->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        tick({first.get(), second.get(), copy.get()}, 2);
        check(second->identity().session == b.session && second->identity().instance == b.instance,
              "state loaded into a running instance -- a preset, an undo -- keeps its identity");

        {
            //  A host that sets a loading project's state a moment after constructing the plugin.
            //  That state must still decide the session -- which is why the first join waits.
            bambi::PluginState project{bambi::encodeParams()};
            bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                             std::string_view(static_cast<const char*>(saved.getData()), saved.getSize()), project);
            project.identity.session = bambi::Uuid::generate();
            project.identity.instance = bambi::Uuid::generate();
            const std::string text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), project);
            auto late = std::make_unique<BambiEncoderProcessor>();
            tick({late.get()}, 1);
            late->setStateInformation(text.data(), static_cast<int>(text.size()));
            tick({late.get()}, joinTicks);
            check(late->linkStatus().open && late->identity().session == project.identity.session &&
                      late->identity().instance == project.identity.instance,
                  "a project's state arriving a tick after construction still decides the session");
            late.reset();
            bambi::LinkBus::unlinkSession(project.identity.session);

            /*  A host that sets a default state and then the project's, both before the join. The
                last one is the project. Catches the first load closing the door. */
            bambi::PluginState earlier = project;
            earlier.identity.session = bambi::Uuid::generate();
            earlier.identity.instance = bambi::Uuid::generate();
            const std::string first_ = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), earlier);
            auto twice = std::make_unique<BambiEncoderProcessor>();
            twice->setStateInformation(first_.data(), static_cast<int>(first_.size()));
            twice->setStateInformation(text.data(), static_cast<int>(text.size()));
            check(twice->identity().instance == project.identity.instance,
                  "of two states loaded before joining, the last decides who this is");
            twice.reset();

            /*  A path is on the bus before anything has been edited. The first snapshot is built
                through a virtual the constructor of the shared base cannot call, so it is built by
                `begin()` -- and the message thread's copy of the path is set from it there. Catches
                that going missing: a fresh encoder then publishing no path until its first edit,
                and every other window drawing a source with nothing under it. */
            //  No state is loaded into it: a load is a commit, and would set the path itself. It
            //  joins the session already here, as a new track does, and `second` is who sees it.
            auto fresh = std::make_unique<BambiEncoderProcessor>();
            tick({first.get(), second.get(), copy.get(), fresh.get()}, joinTicks + 4);
            bambi::LinkScene seenBy;
            second->updateScene(seenBy);
            std::uint32_t points = 0;
            bool found = false;
            for (const auto& entry : seenBy.entries())
                if (entry.instance == fresh->identity().instance) {
                    found = true;
                    points = entry.st.pointCount;
                }
            check(found && points > 0, "a fresh encoder's path is on the bus (" +
                                           juce::String(static_cast<int>(points)) + " points" +
                                           (found ? "" : ", not seen") + ")");
            fresh.reset();
            tick({first.get(), second.get(), copy.get()}, 2);
        }

        bambi::LinkBus remote;  // another instance's window, in effect
        const bool remoteOpen = remote.open(a.session, bambi::Uuid::generate(), bambi::Product::Encoder);
        auto* speed = findParam(*first, "motion.speed");
        GestureRecorder recorder;
        recorder.index = speed != nullptr ? speed->getParameterIndex() : -1;
        first->addListener(&recorder);
        const auto send = [&](float value, std::uint32_t gesture) {
            bambi::LinkCommand command;
            command.param = static_cast<std::uint32_t>(bambi::EncoderParam::MotionSpeed);
            command.value = value;
            command.gesture = gesture;
            return remote.sendCommand(a.instance, command);
        };
        const bool sent = send(10.0f, bambi::kLinkGestureBegin) && send(50.0f, bambi::kLinkGestureNone) &&
                          send(90.0f, bambi::kLinkGestureEnd);
        tick({first.get()}, 1);
        first->removeListener(&recorder);
        const float landed = speed != nullptr ? speed->convertFrom0to1(speed->getValue()) : -1.0f;
        check(remoteOpen && sent && recorder.begins == 1 && recorder.ends == 1 && recorder.values == 3 &&
                  std::abs(landed - 90.0f) < 0.01f,
              "a remote edit reaches the host as one gesture on the target's own parameter, ending at " +
                  juce::String(landed, 2) + " (begins " + juce::String(recorder.begins) + ", values " +
                  juce::String(recorder.values) + ", ends " + juce::String(recorder.ends) + ")");

        {
            //  Another instance's window controlling this one, through the editor's own RemoteTarget: what it
            //  reads, what it sends, and what the target does with it.
            bambi::LinkScene scene;
            bambi::host::RemoteTarget control(*second);
            const auto follow = [&] {
                second->updateScene(scene);
                if (const auto* entry = scene.find(a.instance)) control.follow(*entry, 1.0 / bambi::ui::kFrameHz);
            };
            const auto savedState = [&first] {
                juce::MemoryBlock block;
                first->getStateInformation(block);
                bambi::PluginState s{bambi::encodeParams()};
                bambi::loadState(bambi::Product::Encoder, bambi::encodeParams(),
                                 std::string_view(static_cast<const char*>(block.getData()), block.getSize()), s);
                return s;
            };
            const auto depth = [](const bambi::PluginState& s) {
                return bambi::cellDepth(s, bambi::MatrixTab::Features, 0, bambi::EncoderParam::RenderWidth);
            };
            const auto setDepth = [](double d) {
                return [d](bambi::PluginState& s) {
                    bambi::setCellDepth(bambi::encodeMod(), s, bambi::MatrixTab::Features, 0,
                                        bambi::EncoderParam::RenderWidth, d);
                };
            };
            const auto speedAt = static_cast<std::size_t>(bambi::EncoderParam::MotionSpeed);

            tick({first.get(), second.get()}, 1);
            follow();
            check(control.ready() && std::abs(control.state().params[speedAt] - 90.0f) < 0.01f,
                  "another window reads this instance's controls: the speed the remote gesture left is there");

            control.edit("matrix depth", setDepth(0.5));
            const bool shownAtOnce = std::abs(depth(control.state()) - 0.5) < 1e-12;
            tick({first.get()}, 1);
            const auto landed = savedState();
            check(shownAtOnce && std::abs(depth(landed) - 0.5) < 1e-12 && landed.identity.instance == a.instance &&
                      std::abs(landed.params[speedAt] - 90.0f) < 0.01f && first->canUndoEdit(),
                  "a patch edit shows at once where it was made, and lands whole on the target as its own undo step, "
                  "leaving its identity and parameters alone");

            for (int i = 1; i <= 10; ++i) {
                control.editDrag("matrix depth", "cell", setDepth(0.5 + 0.05 * i));
                tick({first.get()}, 1);
                follow();
            }
            control.endDrag();
            tick({first.get(), second.get()}, 1);
            const bool dragged = std::abs(depth(savedState()) - 1.0) < 1e-12;
            control.undo();
            tick({first.get()}, 1);
            check(dragged && std::abs(depth(savedState()) - 0.5) < 1e-12,
                  "a drag from another window is one undo step on the target, and undoes from that window");

            tick({first.get(), second.get()}, 1);
            follow();
            check(std::abs(depth(control.state()) - 0.5) < 1e-12 && control.canRedo(),
                  "and that window sees the undo come back, through the target's published controls");

            for (const double to : {0.8, 0.3}) {
                for (int i = 1; i <= 3; ++i) {
                    control.editDrag("matrix depth", "cell", setDepth(0.5 + (to - 0.5) * i / 3.0));
                    tick({first.get()}, 1);
                    follow();
                }
                control.endDrag();
                tick({first.get(), second.get()}, 1);
                follow();
            }
            control.undo();
            tick({first.get()}, 1);
            check(std::abs(depth(savedState()) - 0.8) < 1e-12,
                  "two drags of one control, one after the other, stay two undo steps");

            control.beginParameter(bambi::EncoderParam::MotionSpeed);
            control.setParameter(bambi::EncoderParam::MotionSpeed,
                                 static_cast<float>(bambi::ui::normalised(bambi::encodeParams(),
                                                                          bambi::EncoderParam::MotionSpeed, 45.0)));
            control.endParameter(bambi::EncoderParam::MotionSpeed);
            tick({first.get()}, 1);
            check(speed != nullptr && std::abs(speed->convertFrom0to1(speed->getValue()) - 45.0f) < 0.01f,
                  "a parameter dragged in another window lands on the target's own host parameter");

            control.learn(2);
            tick({first.get(), second.get()}, 2);
            follow();
            check(first->noteLearnArmed() == 2 && control.learning() == 2,
                  "learn is armed from another window, which sees it listening");
            control.learn(-1);
            tick({first.get()}, 1);

            //  A turn is engine state, not a parameter, so clearing one from another window travels as a command
            //  and waits for the target's audio thread.
            const int pendingBefore = first->rotationZerosPending();
            control.zeroTurn(1);
            tick({first.get()}, 1);
            check(pendingBefore == 0 && first->rotationZerosPending() == 2,
                  "clearing pitch's turn from another window reaches the target as a command");
        }

        {
            /*  Stepping through the sources from the tab itself, without going back to the matrix.
                The last one is the region, since it has a page of its own. */
            BambiEncoderProcessor solo;
            std::unique_ptr<juce::AudioProcessorEditor> editor(solo.createEditor());
            auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
            bool stepped = false, wrapped = false, followed = false;
            if (shell != nullptr) {
                shell->showSource(0);  // level, on the features tab
                stepped = shell->touchSourceStep(true) && shell->shownSource() == 1;
                shell->showSource(bambi::kNumSources - 1);  // the region, the last source
                wrapped = shell->touchSourceStep(true) && shell->shownSource() == 0;
                shell->showSource(5);  // high, the last feature
                followed = shell->touchSourceStep(true) && shell->shownSource() == 6;
            }
            check(stepped && wrapped && followed,
                  "prev and next step through the sources in matrix order, wrapping at the ends");
        }

        PlayingHead head;
        first->setPlayHead(&head);
        first->setRateAndBufferSizeDetails(kRate, 512);
        first->prepareToPlay(kRate, 512);
        juce::AudioBuffer<float> buffer(std::max(first->getTotalNumInputChannels(), first->getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        for (int block = 0; block < 100; ++block) {
            buffer.clear();
            for (int i = 0; i < 512; ++i)
                buffer.setSample(0, i, 0.3f * std::sin(0.05f * static_cast<float>(block * 512 + i)));
            head.time = static_cast<juce::int64>(block) * 512;
            first->processBlock(buffer, midi);
        }
        tick({first.get(), second.get(), copy.get()}, 1);

        std::vector<bambi::LinkPeer> peers;
        remote.poll(peers);
        const auto seen = std::find_if(peers.begin(), peers.end(),
                                       [&](const bambi::LinkPeer& peer) { return peer.instance == a.instance; });
        //  The angle between the two, from atan2 of the cross and dot products of normalised vectors. A plain
        //  dot product of float vectors cannot resolve 0.01 deg: near 1, acos turns their rounding into ~0.02.
        double angleDeg = 180.0;
        if (seen != peers.end() && seen->hasPosition) {
            const auto& diag = first->diagnostics();
            const double az = diag.azimuthDeg.load() * bambi::kDeg2Rad, el = diag.elevationDeg.load() * bambi::kDeg2Rad;
            const bambi::Vec3 rendered{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
            const auto published = bambi::unit(seen->dyn.position());
            angleDeg = std::atan2(bambi::length(bambi::cross(rendered, published)), bambi::dot(rendered, published)) *
                       bambi::kRad2Deg;
        }
        check(angleDeg < 0.01,
              "the scene sees where the source is: the direction the encoder renders, within 0.01 deg (" +
                  juce::String(angleDeg, 5) + " deg" +
                  (seen != peers.end() && seen->hasPosition ? "" : ", no position") + ")");
        check(seen != peers.end() &&
                  juce::exactlyEqual(seen->dyn.sources[12], first->diagnostics().sourceValues[12].load()) &&
                  seen->dyn.sources[12] != 0.0f,
              "and what each source is sending, for another window's source settings");
        check(seen != peers.end() && seen->dyn.sampledUs > 0 && seen->dyn.publishedUs >= seen->dyn.sampledUs,
              "and when that position was computed, and when it was published");
        bambi::LinkStatic path;
        std::uint32_t generation = 0;
        const bool gotPath = seen != peers.end() && remote.readStatic(*seen, path, generation);
        check(gotPath && generation > 0 && path.order == 3 &&
                  path.pointCount >= static_cast<std::uint32_t>(bambi::kLinkMinPathPoints) &&
                  path.pointCount <= static_cast<std::uint32_t>(bambi::kLinkPathPoints),
              "and its path, with the order it renders at (" +
                  juce::String(gotPath ? static_cast<int>(path.order) : -1) + ")");

        copy.reset();
        remote.poll(peers);
        check(peers.size() == 3, "an instance that closes leaves every scene at once (" +
                                     juce::String(static_cast<int>(peers.size())) +
                                     " live: two instances and the remote)");

        bambi::LinkDirectory directory(directoryName);
        const bool directoryOpen = directory.open();
        remote.close();
        second.reset();
        const bool listed = directoryOpen && directory.mostRecent(bambi::linkNowMicros()) == a.session;
        first.reset();
        const bool unlisted = directoryOpen && !directory.mostRecent(bambi::linkNowMicros()).has_value();
        check(listed && unlisted,
              "the last instance to close unlists its session, so the next new project starts its own");
        bambi::LinkBus::unlinkSession(a.session);
    }
    bambi::LinkDirectory::unlink(directoryName);

    std::printf("\nthe scene's mouse, through the painted regions\n");
    {
        /*  The scene's geometry is checked in core -- `pick`, `pickNode`, `nearestOnCurveScreen`. What
            is checked here is the wiring: that a click selects, a drag orbits and does not select, a
            press on a node starts an edit -- the translation from registered regions into those
            actions. */
        const int joinTicks = BambiEncoderProcessor::kLinkTickHz / 10 + 2;
        auto first = std::make_unique<BambiEncoderProcessor>();
        auto second = std::make_unique<BambiEncoderProcessor>();
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }
        //  turn the second's whole trajectory, so the two dots are nowhere near each other
        if (auto* yaw = second->hostParameter(bambi::EncoderParam::TransformYaw))
            yaw->setValueNotifyingHost(yaw->convertTo0to1(120.0f));
        //  and run them: a position is published by the control step, not by joining the bus
        for (auto* p : {first.get(), second.get()}) {
            p->setRateAndBufferSizeDetails(kRate, 512);
            p->prepareToPlay(kRate, 512);
            juce::AudioBuffer<float> buffer(std::max(p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            for (int block = 0; block < 8; ++block) {
                buffer.clear();
                p->processBlock(buffer, midi);
            }
        }
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }

        std::unique_ptr<juce::AudioProcessorEditor> editor(first->createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell == nullptr) {
            check(false, "no editor: the scene mouse checks cannot run");
        } else {
            shell->tick();
            shell->paintNow();

            //  ---- a click selects the instance under it ------------------------------------
            const auto other = second->identity().instance;
            shell->selectInstance(first->identity().instance);
            shell->tick();
            shell->paintNow();
            //  where the other instance's dot really is, on the equirect, where nothing is hidden
            const auto otherAt = shell->scenePositionOf(other);
            shell->clickScene(otherAt, /*onGlobe*/ false);
            shell->tick();
            /*  Catches the click branch being lost, and catches a click that selects from the wrong
                position -- the press's, not the release's. */
            check(shell->sceneSelected() == other, "a click on a source selects that instance");

            //  ---- a drag orbits the globe, and never selects --------------------------------
            shell->selectInstance(first->identity().instance);
            shell->tick();
            const auto before = shell->sceneCamera();
            shell->dragScene(shell->scenePositionOf(other), 40.0f, 20.0f, /*onGlobe*/ true);
            shell->tick();
            const bool turned = !juce::exactlyEqual(before.yaw, shell->sceneCamera().yaw) ||
                                !juce::exactlyEqual(before.pitch, shell->sceneCamera().pitch);
            check(turned, "a drag on the globe orbits it");

            /*  Each move carries its own delta, not its offset from the press: the same travel in
                four moves turns the globe the same as in one. Reporting the offset instead makes the
                globe run away under the hand, faster the more events the host delivers -- and a
                one-step drag cannot see it at all. */
            const auto beforeOne = shell->sceneCamera();
            shell->dragScene(shell->scenePositionOf(other), 40.0f, 0.0f, true);
            shell->tick();
            const auto oneStep = shell->sceneCamera().yaw - beforeOne.yaw;
            const auto beforeFour = shell->sceneCamera();
            shell->dragSceneSteps(shell->scenePositionOf(other), 40.0f, 0.0f, 4, true);
            shell->tick();
            const auto fourSteps = shell->sceneCamera().yaw - beforeFour.yaw;
            //  Four sums round differently from one, and differently per compiler; the fault is several times the turn.
            check(std::abs(fourSteps - oneStep) < 1e-6,
                  "and the same travel in four moves turns it the same as in one (" + juce::String(fourSteps, 9) +
                      " against " + juce::String(oneStep, 9) + ")");
            /*  A source is never dragged. Catches the drag branch falling through to the click one. */
            check(shell->sceneSelected() == first->identity().instance,
                  "and it selects nothing, however it started on a source");

            //  ---- the equirect does not orbit: it is the whole sphere ----------------------
            const auto flat = shell->sceneCamera();
            shell->dragScene(shell->scenePositionOf(other), 40.0f, 20.0f, /*onGlobe*/ false);
            shell->tick();
            check(juce::exactlyEqual(flat.yaw, shell->sceneCamera().yaw) &&
                      juce::exactlyEqual(flat.pitch, shell->sceneCamera().pitch),
                  "while a drag on the equirect turns nothing");

            /*  ---- and a press the chain takes does not also select -------------------------
                With a custom trajectory the scene edits nodes, and a press that lands on one is an
                edit, not a selection: doing both at once would select whatever sits behind the node.
                This is the branch `pressConsumed_` guards. */
            {
                auto state = bambi::host::LocalTarget(*first).state();
                bambi::convertToCustom(state.trajectory, 8);
                state.trajectory.kind = bambi::TrajectoryKind::Custom;
                const auto text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), state);
                first->setStateInformation(text.data(), static_cast<int>(text.size()));
            }
            shell->showControls(0, bambi::MatrixTab::Features);
            shell->selectInstance(first->identity().instance);
            shell->tick();
            shell->paintNow();
            check(shell->sceneIsEditing(), "a custom trajectory puts the scene into node editing");

            const auto nodeAt = shell->sceneNodePosition(0);
            const auto selectedBefore = shell->sceneSelected();
            shell->clickScene(nodeAt, /*onGlobe*/ false);
            shell->tick();
            check(shell->sceneSelectedNode() == 0, "a click on a node selects that node");
            /*  And it does not also select an instance. This check does not on its own distinguish
                whether the `pressConsumed_` guard is doing the work: a node always sits on its own
                instance's path, so `instanceAt` returns the instance that is already selected even
                without the guard. The guard stays because a press should do one thing, not two. */
            check(shell->sceneSelected() == selectedBefore, "and the instance selection is left alone");
        }
        bambi::LinkBus::unlinkSession(first->identity().session);
    }

    std::printf("\nthe window lists its own kind, not everything on the bus\n");
    {
        /*  One bus carries all three plugins. An instance in the header's list is one this window
            can select and edit, and across kinds that is meaningless -- a parameter position means
            a different parameter in a different plugin, so an edit addressed across kinds already
            moves nothing. Listing one offers an edit that cannot be made.

            Found in a real REAPER project: an encoder beside a Reverb showed two other instances. */
        const int joinTicks = BambiEncoderProcessor::kLinkTickHz / 10 + 2;
        auto encoder = std::make_unique<BambiEncoderProcessor>();
        auto second = std::make_unique<BambiEncoderProcessor>();
        for (int i = 0; i < joinTicks; ++i) {
            encoder->linkTick();
            second->linkTick();
        }
        //  a Reverb on the same bus, joined directly: this target cannot link the other plugins
        bambi::LinkBus reverb;
        const bool reverbOpen =
            reverb.open(encoder->identity().session, bambi::Uuid::generate(), bambi::Product::Reverb);
        reverb.publishStatic({});
        for (int i = 0; i < joinTicks; ++i) {
            encoder->linkTick();
            second->linkTick();
        }
        check(reverbOpen, "a Reverb is on the bus beside two encoders");

        std::unique_ptr<juce::AudioProcessorEditor> editor(encoder->createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell == nullptr) {
            check(false, "no editor: the instance-list check cannot run");
        } else {
            shell->tick();
            /*  Two encoders and a Reverb on one bus: the encoder's window lists the two encoders.
                Catches the filter being absent, which listed all three, and catches one that keeps
                nothing -- the window must still list itself. */
            check(shell->sceneInstanceCount() == 2,
                  "two encoders and a Reverb on the bus: the encoder's window lists the two encoders");
        }
        bambi::LinkBus::unlinkSession(encoder->identity().session);
    }

    std::printf("\na region angle set back to zero takes its turn with it\n");
    {
        BambiEncoderProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 512);
        proc.prepareToPlay(kRate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell == nullptr) {
            check(false, "no editor: the turn checks cannot run");
        } else {
            const auto set = [&](bambi::ParamId id, float value) {
                if (auto* p = proc.hostParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(value));
            };
            //  the region's yaw rate and the placement's, so the two cannot be confused
            set(bambi::parameterByKey("region1.yaw_rate"), 60.0f);
            set(bambi::EncoderParam::TransformYawRate, 60.0f);

            juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                            512);
            juce::MidiBuffer midi;
            /*  The timeline runs on across calls. Restarting it at zero is a locate, and a locate
                resets both clocks -- which would clear the turn for reasons that have nothing to do
                with the double-click, and make every check here pass on its own. */
            juce::int64 at = 0;
            const auto run = [&](int blocks) {
                for (int b = 0; b < blocks; ++b) {
                    buffer.clear();
                    head.time = at;
                    at += 512;
                    proc.processBlock(buffer, midi);
                }
                shell->tick();
            };
            run(60);

            const auto regionYaw = [&] { return proc.regionTurnYaw(); };
            check(regionYaw() > 0.3, "a yaw rate has turned the region");

            /*  And the scene draws it turned. The counter above is the engine's; this is the
                region the last paint drew. Catches the scene drawing the region from its
                parameters as set instead: the audio would move while the picture stood still. */
            shell->chooseRegionKind(static_cast<int>(bambi::RegionKind::Spot));
            run(2);
            check(std::abs(shell->sceneRegionYaw() - regionYaw()) < 0.05 && regionYaw() > 0.3,
                  "and the scene draws the region where the rate has turned it to (" +
                      juce::String(shell->sceneRegionYaw(), 2) + " against " + juce::String(regionYaw(), 2) + ")");

            shell->showSource(bambi::kNumSources - 1);  // its source page, where its tiles are
            shell->setRegionTab(1);                     // on the transform sub-tab
            shell->doubleClickTile(bambi::parameterByKey("region1.yaw"));
            run(2);
            //  Catches the half that was wrong: the angle goes back and the region stays turned.
            check(regionYaw() < 0.05, "and a double-click on its yaw takes the turn with it");

            /*  The placement's turn is not the region's. Catches the encoder's override forwarding a
                region angle to `zeroTurn`, which would clear the placement instead. */
            run(60);
            const auto placementBefore = proc.placementTurnYaw();
            check(placementBefore > 0.3, "the placement's own rate has turned it too");
            shell->doubleClickTile(bambi::parameterByKey("region1.yaw"));
            run(2);
            check(proc.placementTurnYaw() >= placementBefore - 0.05,
                  "and zeroing the REGION's yaw leaves the placement's turn alone");
        }
    }

    std::printf("\nthe region has a source page, and a matrix tab of its own\n");
    {
        BambiEncoderProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get());
        if (shell == nullptr) {
            check(false, "no editor: the region page checks cannot run");
        } else {
            /*  The region is the last source slot. Spelled from the constants rather than as 18, so
                a slot appended after it moves this with the list. */
            const int regionSlot = bambi::kNumSources - 1;
            check(bambi::sourceAt(regionSlot).kind == bambi::SourceKind::Region, "the last source slot is the region");

            const auto offered = shell->matrixTabsOffered();
            check(std::find(offered.begin(), offered.end(), bambi::MatrixTab::Region) != offered.end(),
                  "the encoder's matrix offers the region tab: here the region IS a source");
            /*  And it is clickable where it is drawn. Offering a tab the bar draws but nothing can
                select would look identical from every other check here. */
            shell->clickMatrixTab(bambi::MatrixTab::Features);
            check(
                shell->clickMatrixTab(bambi::MatrixTab::Region) && shell->shownMatrixTab() == bambi::MatrixTab::Region,
                "and clicking it selects it, like the other three");

            //  ---- the page draws the shared editor ------------------------------------------
            /*  Through the column, the way a user opens it: a page nothing can reach is a page that
                is not there. Catches the region's tab being drawn with no column registered on it. */
            shell->showSource(-1);
            shell->clickSourceColumn(bambi::MatrixTab::Region, 0);
            check(shell->shownSource() == regionSlot, "clicking its matrix column opens its settings page");

            /*  One column, not six. Clicking where column 1 would be must open nothing: the tab has
                a single source and five more heads would be five columns standing for nothing.
                Asserted on what the click does, so a view that drew six is caught -- not on a guard
                in the test helper, which would only be testing itself. */
            shell->showSource(-1);
            shell->clickSourceColumn(bambi::MatrixTab::Region, 1);
            const int drewOnRegion = shell->matrixColumnsDrawn();
            shell->clickSourceColumn(bambi::MatrixTab::Features, 0);
            check(drewOnRegion == 1 && shell->matrixColumnsDrawn() == bambi::kSourcesPerTab,
                  "and its tab DRAWS one column where the others draw six");
            /*  Asserted on the drawn count and not only on the click, because five extra heads on
                the region's tab open nothing -- `sourceSlot` refuses a column its tab lacks -- so a
                click cannot tell them from no heads at all. They are still five columns standing
                for nothing. */
            /*  Its parameters are reachable on that page: `region1.size` is a spot's, drawn by the
                shared `paintRegionEditor` and by nothing the encoder wrote. Catches the page being
                registered but left blank. */
            shell->showSource(regionSlot);
            shell->chooseRegionKind(static_cast<int>(bambi::RegionKind::Spot));
            check(shell->drawnSomewhere(bambi::parameterByKey("region1.size")), "a spot's size is drawn on it");
            shell->setRegionTab(1);
            check(shell->drawnSomewhere(bambi::parameterByKey("region1.yaw_rate")) &&
                      !shell->drawnSomewhere(bambi::parameterByKey("region1.size")),
                  "and its yaw rate on the transform sub-tab, without the shape's");
            shell->setRegionTab(0);
            /*  The page follows the kind, which is the whole reason kind is state and not a
                parameter: a band's elevation is not a spot's anything. */
            shell->chooseRegionKind(static_cast<int>(bambi::RegionKind::Band));
            check(shell->drawnSomewhere(bambi::parameterByKey("region1.band_elevation")),
                  "and changing the kind to band puts the band's elevation on it");

            //  ---- stepping reaches it ---------------------------------------------------------
            /*  Stepping through source pages should reach the region, not wrap at the last envelope.
                Catches the step count being left at three tabs, which skips the region entirely. */
            shell->showSource(bambi::kNumSources - 2);  // the last envelope
            check(shell->touchSourceStep(true), "the forward chevron is there on the last envelope");
            check(shell->shownSource() == regionSlot,
                  "and stepping forward from the last envelope reaches the region, not the first feature");
            check(shell->touchSourceStep(true) && shell->shownSource() == 0,
                  "stepping on from the region wraps to the first feature");
            check(shell->touchSourceStep(false) && shell->shownSource() == regionSlot,
                  "and stepping back from the first feature reaches the region");

            /*  ---- the scene corner --------------------------------------------------------
                `regions always` is in the equirect's strip here too, driven through this view's own
                `mouseDown` rather than a `HitArea` -- the encoder's scene is a plain component. The
                click lands where the label is drawn, so a hit area shrunk to the square alone is
                caught rather than followed. */
            shell->chooseRegionKind(static_cast<int>(bambi::RegionKind::Spot));
            shell->showSource(regionSlot);
            shell->paintNow();
            check(shell->regionsDrawnInScene() == 1, "the region is drawn in the scene while its page is open");

            shell->showSource(-1);  // page closed: now it shows only because `regions always` is on
            shell->paintNow();
            check(shell->regionsDrawnInScene() == 1, "and with its page closed, because `regions always` is on");
            shell->clickRegionsAlways();
            shell->paintNow();
            check(shell->regionsDrawnInScene() == 0, "clicking it in the corner hides a region no tab is open on");
            shell->showSource(regionSlot);
            shell->paintNow();
            check(shell->regionsDrawnInScene() == 1, "while the one whose page IS open stays");
            shell->clickRegionsAlways();
            shell->paintNow();

            /*  It flips on release, not on the press. Every clickable in the suite fires on mouseUp
                past the 4 px slop, so a press that drags off can be abandoned; a toggle that flipped
                in `mouseDown` would have no way to abort. */
            shell->showSource(-1);
            shell->paintNow();
            const bool shownBeforeDrag = shell->regionsDrawnInScene() == 1;
            shell->dragOffRegionsAlways(40.0f);
            shell->paintNow();
            check(shownBeforeDrag && shell->regionsDrawnInScene() == 1,
                  "a press that drags off the toggle does not flip it");

            /*  The slop is on the screen. At half size, 6 of the window's units are 3 screen pixels:
                still a click. Measured in the window's own units it was a drag, and 2 screen pixels
                of hand tremor abandoned a click. */
            namespace wm = bambi::ui::theme::metrics;
            shell->setSize(wm::windowWidth / 2, wm::windowHeight / 2);
            shell->paintNow();
            shell->dragOffRegionsAlways(6.0f);
            shell->paintNow();
            check(shell->regionsDrawnInScene() == 0, "at half size, 3 screen pixels of movement is still a click");
            shell->clickRegionsAlways();
            shell->setSize(wm::windowWidth, wm::windowHeight);
            shell->paintNow();
            shell->showSource(regionSlot);
        }
    }

    std::printf("\nthe region in the scene: the shared checks\n");
    bambi::editor::checkRegions<BambiEncoderProcessor>(
        {[](bambi::editor::PluginEditor& e, bool open) {
            if (auto* shell = dynamic_cast<BambiEncoderEditor*>(&e))
                shell->showSource(open ? bambi::kNumSources - 1 : -1);  // the region's source page
        }},
        [](bool ok, const juce::String& what) { check(ok, what); });

    std::printf("\nthe window comes back as it was left\n");
    {
        /*  The view for as long as the plugin is loaded; the size with the session. The window is every
            plugin's, so one plugin's check covers it. Catches the view not kept, the size not kept, and
            either one taken where the other belongs: a reopened project must not bring back a view. */
        namespace wm = bambi::ui::theme::metrics;
        const int w = juce::roundToInt(wm::windowWidth * 0.75f), h = juce::roundToInt(wm::windowHeight * 0.75f);
        const auto open = [](BambiEncoderProcessor& p) {
            return std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
        };
        const auto shellOf = [](const std::unique_ptr<juce::AudioProcessorEditor>& e) {
            return dynamic_cast<BambiEncoderEditor*>(e.get());
        };
        BambiEncoderProcessor proc;
        proc.prepareToPlay(kRate, 512);
        juce::MemoryBlock saved;
        if (auto first = open(proc); shellOf(first) != nullptr) {
            first->setSize(w, h);
            shellOf(first)->clickRegionsAlways();
            check(!shellOf(first)->sceneView().regionsAlways, "regions always, switched off");
        }
        if (auto again = open(proc); shellOf(again) != nullptr) {
            check(again->getWidth() == w && again->getHeight() == h,
                  "reopened, the window has the size it was left at");
            check(!shellOf(again)->sceneView().regionsAlways, "and the view it was left with");
            proc.getStateInformation(saved);
        }
        BambiEncoderProcessor reloaded;
        reloaded.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        reloaded.prepareToPlay(kRate, 512);
        auto fresh = open(reloaded);
        check(shellOf(fresh) != nullptr && fresh->getWidth() == w && fresh->getHeight() == h,
              "a project reopened keeps the size: it is the session's");
        check(shellOf(fresh) != nullptr && shellOf(fresh)->sceneView().regionsAlways,
              "and opens on the default view: that was never saved");
    }

    //  ---- presets: the shared processor's and the shared window's, so the checks are shared too
    std::printf("\npresets\n");
    bambi::editor::checkPresets<BambiEncoderProcessor>({"render.width", "input.mode"},
                                                       [](bool ok, const juce::String& what) { check(ok, what); });

    return checkVerdict();
}
