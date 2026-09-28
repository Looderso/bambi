// SPDX-License-Identifier: GPL-3.0-or-later
#include "Conformance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "bambi/host/Layout.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/math/sh.hpp"

namespace bambi::conformance {

namespace {
constexpr double kRate = 48000.0;
using Layout = juce::AudioProcessor::BusesLayout;
using Channels = std::vector<std::vector<float>>;

struct Run {
    const Subject& subject;
    juce::AudioProcessor::WrapperType format;
    Result result;

    void check(bool ok, const juce::String& what) { report(result, ok, what); }

    /*  A processor made inside this scope reports `format` as its wrapperType, as it does when a
     *  wrapper creates it. Nothing in the rule is format-conditional -- and keeping it that way is
     *  part of what this suite is for, so every check runs under both. */
    std::unique_ptr<juce::AudioProcessor> make() const {
        juce::AudioProcessor::setTypeOfNextNewPlugin(format);
        auto p = subject.make();
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        return p;
    }
};

Layout layout(const juce::AudioChannelSet& in, const juce::AudioChannelSet& sidechain,
              const juce::AudioChannelSet& out) {
    Layout l;
    l.inputBuses.add(in);
    l.inputBuses.add(sidechain);
    l.outputBuses.add(out);
    return l;
}

juce::String describe(const Layout& l) {
    return "in " + juce::String(l.getMainInputChannelSet().size()) + " | sc " +
           juce::String(l.getChannelSet(true, 1).size()) + " | out " + juce::String(l.getMainOutputChannelSet().size());
}

/*  The channel set for an order. JUCE's `ambisonic()` does not go all the way to kMaxHostOrder, and
 *  asking it for one it cannot express returns a set whose size is nonsense -- which a check then
 *  "passes" on a layout no host could ever offer. Above what it knows, a plain discrete width is
 *  what a DAW track gives anyway, and width is all the suite's rule looks at. */
juce::AudioChannelSet amb(int order) {
    const auto set = juce::AudioChannelSet::ambisonic(order);
    return set.size() == bambi::numChannels(order) ? set
                                                   : juce::AudioChannelSet::discreteChannels(bambi::numChannels(order));
}

/*  A deterministic signal: a function of the sample index alone, so two renders at different block
 *  sizes are comparing the same input and nothing else. */
float sample(int channel, juce::int64 at) {
    const double t = static_cast<double>(at) / kRate;
    const double impulse = (at % 8000 == 0) ? 0.8 : 0.0;
    return static_cast<float>((impulse + 0.25 * std::sin(6.2831853 * (110.0 + 40.0 * channel) * t)) /
                              (1.0 + 0.5 * channel));
}

/*  `seconds` through a fresh processor in blocks of `block`, transport running from zero. Returns
 *  the main output's channels. `sidechainOnly` feeds the sidechain bus and leaves the main silent. */
Channels render(const Run& run, const Layout& l, int block, double seconds, bool offline = false,
                bool sidechainOnly = false) {
    auto proc = run.make();
    if (!proc->setBusesLayout(l)) return {};

    bambi::host::TestPlayHead head;
    proc->setPlayHead(&head);
    proc->setNonRealtime(offline);
    proc->setRateAndBufferSizeDetails(kRate, block);
    proc->prepareToPlay(kRate, block);

    const int inChannels = l.getMainInputChannelSet().size();
    const int outChannels = l.getMainOutputChannelSet().size();
    const int sideChannels = l.getChannelSet(true, 1).size();
    const int width = std::max({inChannels + sideChannels, outChannels, 1});
    const auto frames = static_cast<juce::int64>(seconds * kRate);

    Channels captured(static_cast<std::size_t>(outChannels));
    juce::AudioBuffer<float> buffer(width, block);
    juce::MidiBuffer midi;
    for (juce::int64 at = 0; at < frames; at += block) {
        head.time = at;
        buffer.clear();
        if (!sidechainOnly)
            for (int c = 0; c < inChannels; ++c) {
                float* x = buffer.getWritePointer(c);
                for (int i = 0; i < block; ++i) x[i] = sample(c, at + i);
            }
        for (int c = 0; c < sideChannels; ++c) {
            float* x = buffer.getWritePointer(inChannels + c);
            for (int i = 0; i < block; ++i) x[i] = sample(c, at + i);
        }
        proc->processBlock(buffer, midi);
        for (int c = 0; c < outChannels; ++c) {
            const float* y = buffer.getReadPointer(c);
            captured[static_cast<std::size_t>(c)].insert(captured[static_cast<std::size_t>(c)].end(), y, y + block);
        }
    }
    return captured;
}

bool identical(const Channels& a, const Channels& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (std::size_t c = 0; c < a.size(); ++c) {
        const auto n = std::min(a[c].size(), b[c].size());
        if (n == 0) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (a[c][i] != b[c][i]) return false;
    }
    return true;
}

double energy(const Channels& c, std::size_t from = 0) {
    double sum = 0.0;
    for (const auto& ch : c)
        for (std::size_t i = from; i < ch.size(); ++i) sum += static_cast<double>(ch[i]) * ch[i];
    return sum;
}

//  ---- the six ----------------------------------------------------------------------------------

/// 1 · the layouts it claims are accepted, and the ones it does not are refused.
void claimedLayouts(Run& r) {
    auto proc = r.make();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto accepts = [&proc](const Layout& l) { return proc->checkBusesLayoutSupported(l); };

    for (const int order : r.subject.orders) {
        const auto in = r.subject.fieldEffect ? amb(order) : stereo;
        const auto l = layout(in, stereo, amb(order));
        r.check(accepts(l), "order " + juce::String(order) + " accepted: " + describe(l));
    }

    /*  A width that is not a whole order is still accepted on the output and the extra channels left
     *  silent: a REAPER track has only even channel counts, so orders 2, 4, 6, 8 and 10 could never
     *  be asked for otherwise. The encoder alone can take one, since an effect's input must match
     *  its output exactly. */
    if (!r.subject.fieldEffect) {
        const auto l = layout(stereo, stereo, juce::AudioChannelSet::discreteChannels(10));
        r.check(accepts(l), "a width between orders is accepted, the rest left silent: " + describe(l));
    }

    Layout noOutput;
    noOutput.inputBuses.add(r.subject.fieldEffect ? amb(3) : stereo);
    noOutput.inputBuses.add(stereo);
    noOutput.outputBuses.add(juce::AudioChannelSet::disabled());
    r.check(!accepts(noOutput), "no output refused");

    //  A sidechain that is not connected is a layout a host may ask for, and must be accepted.
    Layout noSide;
    noSide.inputBuses.add(r.subject.fieldEffect ? amb(3) : stereo);
    noSide.inputBuses.add(juce::AudioChannelSet::disabled());
    noSide.outputBuses.add(amb(3));
    r.check(accepts(noSide), "a sidechain that is not connected: " + describe(noSide));
}

/// 2 · a wide main input is refused, so a sidechain is never a wide main bus read past its first pair.
void wideMainInput(Run& r) {
    auto proc = r.make();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto accepts = [&proc](const Layout& l) { return proc->checkBusesLayoutSupported(l); };

    if (r.subject.fieldEffect) {
        /*  For a field effect the main input is the field, so the property takes the form "in must
            equal out": a plugin that accepted a different width would be answering a question it
            was not asked, and a host could then hand it a main bus wider than the field. */
        r.check(!accepts(layout(amb(1), stereo, amb(3))),
                "narrower in than out refused: " + describe(layout(amb(1), stereo, amb(3))));
        r.check(!accepts(layout(amb(3), stereo, amb(1))),
                "wider in than out refused: " + describe(layout(amb(3), stereo, amb(1))));
    } else {
        /*  A 16-channel main input on a 16-channel REAPER track takes pins 1-16, so a send into
            track channels 3/4 lands inside it where nothing reads it, and the sidechain bus, which
            begins at pin 17, hears nothing. */
        r.check(!accepts(layout(amb(3), stereo, amb(3))),
                "a main bus that swallows track channels 3/4 refused: " + describe(layout(amb(3), stereo, amb(3))));
        r.check(!accepts(layout(juce::AudioChannelSet::discreteChannels(4), stereo, amb(3))),
                "and any main input wider than stereo");
    }
}

/// 3 · the sidechain arrives, at every order, on its own bus.
void sidechainAtEveryOrder(Run& r) {
    const auto stereo = juce::AudioChannelSet::stereo();
    for (const int order : {1, 3, 5}) {
        const auto in = r.subject.fieldEffect ? amb(order) : stereo;
        const auto l = layout(in, stereo, amb(order));

        /*  Audio on the sidechain bus alone, with the main input silent. What it must not do is
            reach the output as if it were the input: the sidechain drives detectors, never the
            signal path. And the plugin must survive it at every order. */
        const auto out = render(r, l, 128, 0.25, false, true);
        r.check(!out.empty(), "order " + juce::String(order) + ": a sidechain-only block is processed");
        r.check(!out.empty() && energy(out) < 1e-9, "and none of the sidechain's audio comes out of the main bus");
    }
}

/// 4 · channels beyond the order are silent.
void unusedOutputsSilent(Run& r) {
    /*  An in-place host hands over the same memory for input and output channels, so a plugin that
        simply did not write the channels past its order would leave the input's audio in them. */
    const auto stereo = juce::AudioChannelSet::stereo();
    const int order = 2;   // 9 channels
    const int width = 12;  // three spare
    Layout l;
    l.inputBuses.add(r.subject.fieldEffect ? amb(order) : stereo);
    l.inputBuses.add(stereo);
    l.outputBuses.add(juce::AudioChannelSet::discreteChannels(width));

    auto proc = r.make();
    if (!proc->setBusesLayout(l)) {
        /*  A field effect cannot take this layout at all -- its input must match its output, and 12
            is not a whole order. That is a stronger answer than silence, and it is checked above. */
        r.check(r.subject.fieldEffect, "a width between orders is refused outright, which is stronger");
        return;
    }
    const auto out = render(r, l, 128, 0.2);
    const int used = bambi::numChannels(bambi::host::orderThatFits(width));
    bool silent = static_cast<int>(out.size()) == width;
    for (int c = used; c < width && silent; ++c)
        for (const float v : out[static_cast<std::size_t>(c)])
            if (v != 0.0f) {
                silent = false;
                break;
            }
    r.check(silent, "channels " + juce::String(used + 1) + ".." + juce::String(width) + " are silent");
}

/// 5 · output does not depend on the host's block size.
void blockSizeIndependent(Run& r) {
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto l = layout(r.subject.fieldEffect ? amb(3) : stereo, stereo, amb(3));
    const auto reference = render(r, l, 128, 0.5);
    r.check(!reference.empty(), "renders at block 128");
    for (const int block : {1, 64, 333, 1024})
        r.check(identical(reference, render(r, l, block, 0.5)),
                "block " + juce::String(block) + " renders the same samples as 128");
}

/// 6 · an offline bounce is what was heard.
void offlineEqualsRealtime(Run& r) {
    /*  Reverb deliberately renders a better tail offline when it is asked to, which is a setting
        and not a difference in what a block does -- at its default, and for every plugin, offline
        and realtime are the same samples. */
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto l = layout(r.subject.fieldEffect ? amb(3) : stereo, stereo, amb(3));
    const auto realtime = render(r, l, 256, 0.5, false);
    const auto bounced = render(r, l, 512, 0.5, true);
    r.check(identical(realtime, bounced), "an offline bounce is sample for sample what was heard");
}

}  // namespace

Result run(const Subject& subject, juce::AudioProcessor::WrapperType format) {
    Run r{subject, format, {}};
    const char* which = format == juce::AudioProcessor::wrapperType_VST3 ? "VST3" : "CLAP";
    std::printf("\n%s as %s\n", subject.name.c_str(), which);

    claimedLayouts(r);
    wideMainInput(r);
    sidechainAtEveryOrder(r);
    unusedOutputsSilent(r);
    blockSizeIndependent(r);
    offlineEqualsRealtime(r);
    return r.result;
}

}  // namespace bambi::conformance
