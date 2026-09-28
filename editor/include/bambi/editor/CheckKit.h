// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <vector>

#include "bambi/host/TestPlayHead.h"
#include "bambi/patch/state.hpp"

/*  What every plugin's check suite is built from: reporting, layouts, parameters, a deterministic test
    field and renders to compare. A suite is one binary, so the failure count is one per program. */
namespace bambi::editor {

using Layout = juce::AudioProcessor::BusesLayout;
using Channels = std::vector<std::vector<float>>;  ///< per output channel, every sample of a render

inline constexpr double kCheckRate = 48000.0;

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

}  // namespace bambi::editor
