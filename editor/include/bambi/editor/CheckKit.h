// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <vector>

#include "bambi/host/PluginProcessor.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/link/link.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/patch/state.hpp"

/*  What every plugin's check suite is built from: reporting, layouts, parameters, a deterministic test
    field and renders to compare. A suite is one binary, so the failure count is one per program. */
namespace bambi::editor {

using Layout = juce::AudioProcessor::BusesLayout;
using Channels = std::vector<std::vector<float>>;  ///< per output channel, every sample of a render

inline constexpr double kCheckRate = 48000.0;

// ---- a Decoder on the bus ---------------------------------------------------------------------

/*  Stands in for a Decoder in `session`: publishes what a real one does -- its layout in the static section, what it plays and
    where its head looks in the dynamic -- so a plugin that draws decoders is checked on its own side of the bus. `speakers` points
    on a ring, the last one imaginary from four on; `flags` is `LinkDynamic::playFlags`. Released with the bus. */
inline std::unique_ptr<LinkBus> standInDecoder(const Uuid& session, const char* name, int speakers, std::uint8_t flags,
                                               Vec3 gaze) {
    auto bus = std::make_unique<LinkBus>();
    bus->open(session, Uuid::generate(), Product::Decoder);
    LinkStatic st;
    st.setLabel(name);
    st.speakerCount = static_cast<std::uint32_t>(speakers);
    for (int i = 0; i < speakers; ++i) {
        st.speakers[i].azimuthDeg = static_cast<float>(i * 360.0 / std::max(speakers, 1));
        st.speakers[i].elevationDeg = (i % 2) ? 20.0f : 0.0f;
        st.speakers[i].channel = static_cast<std::uint8_t>(i + 1);
        st.speakers[i].imaginary = (i == speakers - 1 && speakers > 3) ? 1 : 0;
    }
    bus->publishStatic(st);
    LinkDynamic d;
    d.playFlags = flags;
    d.headGazeX = static_cast<float>(gaze.x);
    d.headGazeY = static_cast<float>(gaze.y);
    d.headGazeZ = static_cast<float>(gaze.z);
    d.headLeftX = static_cast<float>(-gaze.y);
    d.headLeftY = static_cast<float>(gaze.x);
    d.headLeftZ = 0.0f;
    bus->publishDynamic(d);
    return bus;
}

// ---- reporting --------------------------------------------------------------------------------

inline int& checkFailures() {
    static int failures = 0;
    return failures;
}

/// Prints one result, "ok" or "FAIL", and counts the failures.
inline void check(bool ok, const juce::String& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (!ok) ++checkFailures();
}

/// Prints the verdict and returns the process exit code: 0 when every check passed.
inline int checkVerdict() {
    const int failures = checkFailures();
    std::printf("\n%s\n",
                failures == 0 ? "ALL CHECKS PASS" : (juce::String(failures) + " CHECK(S) FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}

// ---- layouts and parameters -------------------------------------------------------------------

/// A main input, a sidechain and a main output.
inline Layout layout(const juce::AudioChannelSet& in, const juce::AudioChannelSet& sidechain,
                     const juce::AudioChannelSet& out) {
    Layout l;
    l.inputBuses.add(in);
    l.inputBuses.add(sidechain);
    l.outputBuses.add(out);
    return l;
}

inline juce::AudioChannelSet amb(int order) { return juce::AudioChannelSet::ambisonic(order); }

inline juce::String describe(const Layout& l) {
    return "in " + juce::String(l.getMainInputChannelSet().size()) + " | sc " +
           juce::String(l.getChannelSet(true, 1).size()) + " | out " + juce::String(l.getMainOutputChannelSet().size());
}

inline juce::RangedAudioParameter* findParam(juce::AudioProcessor& proc, const juce::String& id) {
    for (auto* param : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            if (ranged->paramID == id) return ranged;
    return nullptr;
}

/// Sets a parameter by key to a plain value, as a host's automation would.
inline void setParameter(juce::AudioProcessor& proc, const juce::String& id, float value) {
    if (auto* ranged = findParam(proc, id)) ranged->setValueNotifyingHost(ranged->convertTo0to1(value));
}

// ---- comparing renders ------------------------------------------------------------------------

/// Bit-identical over the samples both have, and neither is empty.
inline bool identical(const Channels& a, const Channels& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (std::size_t c = 0; c < a.size(); ++c) {
        const auto n = std::min(a[c].size(), b[c].size());
        if (n == 0) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (a[c][i] != b[c][i]) return false;
    }
    return true;
}

/// Where two renders first differ, or empty when they do not.
inline juce::String firstDifference(const Channels& a, const Channels& b) {
    if (a.empty() || b.empty()) return "a render produced nothing (layout refused?)";
    if (a.size() != b.size())
        return "channel counts differ: " + juce::String(static_cast<int>(a.size())) + " vs " +
               juce::String(static_cast<int>(b.size()));
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i)
            if (a[c][i] != b[c][i])
                return "first difference: channel " + juce::String(static_cast<int>(c)) + ", sample " +
                       juce::String(static_cast<juce::int64>(i));
    return {};
}

inline double energy(const Channels& c) {
    double sum = 0.0;
    for (const auto& ch : c)
        for (const float v : ch) sum += static_cast<double>(v) * v;
    return sum;
}

inline float peak(const Channels& x) {
    float p = 0.0f;
    for (const auto& ch : x)
        for (float v : ch) p = std::max(p, std::abs(v));
    return p;
}

// ---- rendering a field through an effect ------------------------------------------------------

/*  A deterministic field: an impulse train and a tone, weighted per channel. A function of the sample
    index alone, so renders at different block sizes compare the same input. */
inline float fieldSample(int channel, juce::int64 at) {
    const double t = static_cast<double>(at) / kCheckRate;
    const double impulse = (at % 12000 == 0) ? 1.0 : 0.0;
    const double tone = 0.25 * std::sin(6.2831853 * 220.0 * t);
    const double weight = 1.0 / (1.0 + 0.7 * channel);
    return static_cast<float>((impulse + tone) * weight);
}

/// Where a render starts, a locate partway through it, and what it is loaded with.
struct RenderRun {
    std::string state;                      ///< setStateInformation before preparing, when not empty
    juce::int64 locateAt{-1}, locateTo{0};  ///< at output sample `locateAt`, the timeline jumps to `locateTo`
    juce::int64 startAt{0};                 ///< where the timeline starts
    const PluginState* patch{nullptr};      ///< committed after preparing, when set
};

/*  `seconds` of the test field through a fresh `Processor` in blocks of `block`, returning its main output.
    `setUp` sets the parameters the suite renders with, after any state and before preparing. */
template <class Processor>
Channels renderField(const Layout& l, int block, double seconds, const std::function<void(Processor&)>& setUp,
                     const RenderRun& run = {}) {
    Processor proc;
    if (!proc.setBusesLayout(l)) return {};
    host::TestPlayHead head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(kCheckRate, block);
    if (!run.state.empty()) proc.setStateInformation(run.state.data(), static_cast<int>(run.state.size()));
    if (setUp) setUp(proc);
    proc.prepareToPlay(kCheckRate, block);
    if (run.patch != nullptr) proc.commitDocument(*run.patch, true);

    const int inChannels = l.getMainInputChannelSet().size();
    const int outChannels = l.getMainOutputChannelSet().size();
    const int sideChannels = l.getChannelSet(true, 1).size();
    const int width = std::max({inChannels + sideChannels, outChannels, 1});
    const auto frames = static_cast<juce::int64>(seconds * kCheckRate);

    Channels captured(static_cast<std::size_t>(outChannels));
    juce::AudioBuffer<float> buffer(width, block);
    juce::MidiBuffer midi;
    juce::int64 timeline = run.startAt;
    for (juce::int64 at = 0; at < frames; at += block) {
        if (run.locateAt >= 0 && at == run.locateAt) timeline = run.locateTo;
        head.time = timeline;
        buffer.clear();
        for (int c = 0; c < inChannels; ++c) {
            float* x = buffer.getWritePointer(c);
            for (int i = 0; i < block; ++i) x[i] = fieldSample(c, timeline + i);
        }
        proc.processBlock(buffer, midi);
        for (int c = 0; c < outChannels; ++c) {
            const float* y = buffer.getReadPointer(c);
            captured[static_cast<std::size_t>(c)].insert(captured[static_cast<std::size_t>(c)].end(), y, y + block);
        }
        timeline += block;
    }
    return captured;
}

/*  Every parameter's text, as a host shows it, reads back as the value it names: value to text to value to text
    gives the same text, at several points of each range. Catches text written finer than the value a host stores,
    which a host's text field, and the CLAP validator, see drift in its last digit. */
template <class Processor>
void checkParameterText() {
    std::printf("\nevery parameter's text reads back as the value it names\n");
    Processor proc;
    int unstable = 0;
    juce::String first;
    for (auto* p : proc.getParameters()) {
        for (const float at : {0.0f, 0.1f, 0.3461538f, 0.4242424f, 0.5f, 0.77f, 1.0f}) {
            //  as bytes, the way a CLAP wrapper passes a host's text back: not read as UTF-8
            const juce::String text = p->getText(at, 64);
            const juce::String again = p->getText(p->getValueForText(juce::String(text.toRawUTF8())), 64);
            if (text != again) {
                if (unstable++ == 0) first = p->getName(64) + ": " + text + " -> " + again;
            }
        }
    }
    check(unstable == 0, unstable == 0 ? juce::String("every parameter, at seven points of its range")
                                       : juce::String(unstable) + " unstable, first " + first);
}

/*  A host's save and reload gives back what it saved: every parameter set to a value of its range, as a host sets
    one, a block processed, the state saved, and a fresh instance loaded from it reads every parameter as it was;
    saving that instance again gives the same bytes, and the load tells the host every value may have moved.
    Catches a parameter the state leaves out or reads back to another value, a load that applies its values late or
    not to the host's parameters, a save that writes a reloaded value a rounding away, and a load the host is not
    told of -- a CLAP host would keep showing the old values. */
template <class Processor>
void checkStateRoundTrip(const Layout& l) {
    std::printf("\nthe state a host saves reloads every parameter as it was\n");
    Processor a;
    a.setBusesLayout(l);
    a.setRateAndBufferSizeDetails(kCheckRate, 128);
    a.prepareToPlay(kCheckRate, 128);
    juce::Random random(7);
    for (auto* p : a.getParameters()) {
        if (p == a.getBypassParameter()) continue;
        //  a stepped parameter at one of its steps, as a host sets it
        const int steps = p->getNumSteps();
        const float r = random.nextFloat();
        const bool discrete = p->isDiscrete() && steps > 1 && steps < 0x7fffffff;
        p->setValueNotifyingHost(
            discrete ? std::round(r * static_cast<float>(steps - 1)) / static_cast<float>(steps - 1) : r);
    }
    juce::AudioBuffer<float> buffer(std::max(l.getMainInputChannelSet().size() + l.getChannelSet(true, 1).size(),
                                             l.getMainOutputChannelSet().size()),
                                    128);
    buffer.clear();
    juce::MidiBuffer midi;
    a.processBlock(buffer, midi);
    juce::MemoryBlock saved;
    a.getStateInformation(saved);
    Processor b;
    struct Told : juce::AudioProcessorListener {
        int programs = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override {
            programs += d.programChanged ? 1 : 0;
        }
    } told;
    b.addListener(&told);
    b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    b.removeListener(&told);
    check(told.programs > 0, "the host is told every value may have moved");
    int moved = 0;
    juce::String first;
    const auto& pa = a.getParameters();
    const auto& pb = b.getParameters();
    for (int i = 0; i < std::min(pa.size(), pb.size()); ++i)
        if (std::abs(pa[i]->getValue() - pb[i]->getValue()) > 1.0e-6f && moved++ == 0)
            first = pa[i]->getName(64) + " " + pa[i]->getText(pa[i]->getValue(), 32) + " -> " +
                    pb[i]->getText(pb[i]->getValue(), 32);
    check(moved == 0,
          moved == 0 ? juce::String("every parameter as it was") : juce::String(moved) + " moved, first " + first);
    juce::MemoryBlock again;
    b.getStateInformation(again);
    check(again == saved, "and saved again, the same bytes");
}

/*  The host's bypass is the plugin's own parameter, keyed `bypass`: switched on a quarter-second in and
    off at three quarters, the output fades over `PluginProcessor::kBypassFade` to what `bypassed` says the
    plugin plays for the test field (main output channel, sample), and back to the engine. */
template <class Processor>
void checkBypass(const Layout& l, const char* what, const std::function<float(int, juce::int64)>& bypassed) {
    std::printf("\nthe host's bypass is the plugin's own parameter, and bypassed it plays %s\n", what);
    Processor proc;
    if (!proc.setBusesLayout(l)) {
        check(false, "the layout is accepted");
        return;
    }
    auto* param = dynamic_cast<juce::HostedAudioProcessorParameter*>(proc.getBypassParameter());
    check(param != nullptr && param->getParameterID() == "bypass", "the bypass parameter is declared, keyed bypass");
    if (param == nullptr) return;

    const int inChannels = l.getMainInputChannelSet().size();
    const int outChannels = l.getMainOutputChannelSet().size();
    const int width = std::max({inChannels + l.getChannelSet(true, 1).size(), outChannels, 1});
    //  Switches whose blocks, of 128 and of 100, start between the same two points of the grid.
    const juce::int64 on = 12037, off = 36237, frames = static_cast<juce::int64>(kCheckRate);
    //  A render with the bypass switched on at sample `on` and off at `off`, or at the first block after.
    const auto render = [&](Processor& proc, int block) {
        host::TestPlayHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kCheckRate, block);
        proc.prepareToPlay(kCheckRate, block);
        auto* bypass = proc.getBypassParameter();
        bypass->setValueNotifyingHost(0.0f);
        Channels out(static_cast<std::size_t>(outChannels));
        juce::AudioBuffer<float> buffer(width, block);
        juce::MidiBuffer midi;
        for (juce::int64 at = 0; at < frames; at += block) {
            if (at <= on && on < at + block) bypass->setValueNotifyingHost(1.0f);
            if (at <= off && off < at + block) bypass->setValueNotifyingHost(0.0f);
            head.time = at;
            buffer.clear();
            for (int c = 0; c < inChannels; ++c)
                for (int i = 0; i < block; ++i) buffer.setSample(c, i, fieldSample(c, at + i));
            proc.processBlock(buffer, midi);
            for (int c = 0; c < outChannels; ++c) {
                const float* y = buffer.getReadPointer(c);
                out[static_cast<std::size_t>(c)].insert(out[static_cast<std::size_t>(c)].end(), y, y + block);
            }
        }
        return out;
    };
    const Channels out = render(proc, 128);

    const auto distance = [&](juce::int64 i) {
        float d = 0.0f;
        for (int c = 0; c < outChannels; ++c)
            d = std::max(d, std::abs(out[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] - bypassed(c, i)));
        return d;
    };
    const juce::int64 grid = host::PluginProcessor::kBypassGrid;
    const auto fade = static_cast<juce::int64>(std::ceil(host::PluginProcessor::kBypassFade * kCheckRate));
    //  Set at the start of the block holding `on`; the fade starts on the grid after that.
    const juce::int64 startsOn = (on / 128 * 128 + grid - 1) / grid * grid;
    const juce::int64 startsOff = (off / 128 * 128 + grid - 1) / grid * grid;
    float held = 0.0f;
    for (juce::int64 i = startsOn + fade; i < startsOff; ++i) held = std::max(held, distance(i));
    //  Catches: a bypassed output that is not the plugin's own (the input handed back to a Decoder, the Encoder's
    //  other channels left as they arrived).
    check(held < 1e-6f, "once faded in, every channel is exactly that");
    float fading = 0.0f;
    for (juce::int64 i = startsOn + 1; i < startsOn + fade / 2; ++i) fading = std::max(fading, distance(i));
    //  Catches: a switch that steps rather than fades.
    check(fading > 1e-4f, "it fades in rather than stepping");
    float back = 0.0f;
    for (juce::int64 i = startsOff + fade; i < frames; ++i) back = std::max(back, distance(i));
    //  Catches: a bypass that latches.
    check(back > 1e-4f, "and switched off, the engine plays again");

    //  Catches: a fade that starts where the host's block does, so a bounce with the bypass automated
    //  depends on the block size.
    Processor other;
    other.setBusesLayout(l);
    check(identical(out, render(other, 100)), "and a block of 100 switches it on the same samples as 128");
}

}  // namespace bambi::editor
