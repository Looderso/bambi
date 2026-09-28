// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/FieldEffectProcessor.h"

#include <algorithm>

#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"

namespace bambi::host {

namespace {
constexpr int kDefaultOrder = 3;
}  // namespace

FieldEffectProcessor::FieldEffectProcessor(const Spec& spec)
    : PluginProcessor({spec.product, spec.params, spec.mod, spec.regionSlots, spec.factory},
                      BusesProperties()
                          .withInput("Ambisonic", juce::AudioChannelSet::ambisonic(kDefaultOrder), true)
                          .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Ambisonic", juce::AudioChannelSet::ambisonic(kDefaultOrder), true),
                      diag_),
      controlHop_(spec.controlHop) {}

bool FieldEffectProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    //  The field arrives and the same field leaves, at the same order, unlike the encoder's
    //  "mono or stereo in, any width out".
    return fieldEffectLayoutSupported(layouts);
}

void FieldEffectProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto maxBlock = static_cast<std::size_t>(std::max(1, maximumExpectedSamplesPerBlock));

    engineOrder_ = orderThatFits(getChannelCountOfBus(false, 0));

    //  Everything that allocates happens here and nowhere else.
    if (engineOrder_ >= 0) prepareEngine(engineOrder_, sampleRate_);
    input_.prepare(sampleRate_, static_cast<int>(maxBlock));
    diag_.sampleRate.store(sampleRate_, std::memory_order_relaxed);
    modulation().prepare(sampleRate_);
    setLoadRate(sampleRate_);
    //  One control hop is the longest segment the render loop can produce, by construction.
    field_.prepare(controlHop_, engineOrder_);

    prepareEnergy(engineOrder_, sampleRate_, true);  // tracks what arrived, and what leaves

    transport_.forget();
    grid_.forget();
    restartAt(0, false);
}

void FieldEffectProcessor::snapshotAdopted(const EngineSnapshot& next) noexcept {
    if (next.snap) {
        grid_.forceStep();
        modulation().settle();  // a state loaded whole is heard whole, not glided into
    }
}

void FieldEffectProcessor::restartAt(std::int64_t timelineSample, bool fromTransport) noexcept {
    resetEngine();
    input_.reset();
    resetEnergy();

    /*  What a plugin holds across steps goes back, so a render repeats -- except a region's turn
        when `rates.retrigger` says continue, since the user may want a bounce to differ there.
        An LFO's own switch is the modulation engine's. */
    if (fromTransport) {
        restartControl(true, sharedValue(ratesRetriggerAt(), 0.0f) > 0.5f);
        modulation().restartTransport();
    } else {
        restartControl(false, false);
        modulation().reset();
    }

    /*  The control grid is pinned to the timeline, not to wherever processing began: playback and a
        bounce that start at the same place step at the same samples, whatever block sizes the host
        uses for each. */
    grid_.restartAt(timelineSample, controlHop_);
}

// ---- audio ---------------------------------------------------------------------------

void FieldEffectProcessor::renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    bambi::ScopedFlushDenormals flushDenormals;  // the render tools' environment too
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    //  A state load posted since the last block takes effect here, at the boundary.
    adoptPendingSnapshot();
    catchLearnedNote(midi);

    //  Parameters: one lock-free copy per block, handed to the engine without allocating.
    intakeParameters();
    //  How long each Level takes to fall, by its place in the list: nothing to look up here.
    input_.setLevelRelease(sharedValue(levelReleaseAt(), 200.0f), sharedValue(scLevelReleaseAt(), 200.0f));

    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    const int outChannels = out.getNumChannels();

    diag_.mainInputChannels.store(in.getNumChannels(), std::memory_order_relaxed);
    diag_.outputChannels.store(outChannels, std::memory_order_relaxed);
    diag_.order.store(orderThatFits(outChannels), std::memory_order_relaxed);
    diag_.nonRealtime.store(isNonRealtime(), std::memory_order_relaxed);

    if (engineOrder_ < 0 || orderThatFits(outChannels) != engineOrder_) {
        //  The layout changed without a prepareToPlay. Silence rather than a guess.
        buffer.clear();
        grid_.pass(numSamples, controlHop_, midi);
        diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
        diag_.layoutMismatches.fetch_add(1, std::memory_order_relaxed);
        diag_.outputPeak.store(0.0f, std::memory_order_relaxed);
        return;
    }

    //  W, the field's power, the sidechain and the detectors they feed are the shared field input.
    const bool haveSidechain =
        getBusCount(true) > 1 && getBus(true, 1)->isEnabled() && getChannelCountOfBus(true, 1) > 0;
    const auto side = haveSidechain ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<float>();
    if (!input_.take(in, haveSidechain ? &side : nullptr, numSamples, std::max(0, engineOrder_))) {
        //  A host exceeding its own announced maximum. Silence, never a resize on this thread.
        buffer.clear();
        grid_.pass(numSamples, controlHop_, midi);
        diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
        return;
    }
    diag_.inputPeak.store(input_.peak(), std::memory_order_relaxed);
    diag_.sidechainPeak.store(input_.sidechainPeak(), std::memory_order_relaxed);
    diag_.sidechainChannels.store(haveSidechain ? side.getNumChannels() : 0, std::memory_order_relaxed);

    /*  Where the host is, and whether it jumped, decided the same way everywhere: two plugins that
        disagreed about where a timeline jumped would put two different renders in the same bounce. */
    const auto tp = transport_.observe(getPlayHead(), numSamples,
                                       static_cast<std::int64_t>(input_.capacity()) + kTimelineSlack, sampleRate_);
    diag_.playing.store(tp.playing, std::memory_order_relaxed);
    diag_.transportKnown.store(tp.timeline >= 0, std::memory_order_relaxed);
    diag_.timeInSamples.store(tp.timeline, std::memory_order_relaxed);
    diag_.bpm.store(tp.bpm, std::memory_order_relaxed);
    if (tp.restart) restartAt(tp.start, true);

    /*  The field is read and the field is written; an engine is in-place safe. The channels past the
        order are cleared, as the encoder clears them: they are not the field, and on an in-place
        host they still hold whatever arrived -- so leaving them passes audio this plugin never
        processed straight through to whatever sums the bus. */
    for (int c = numChannels(engineOrder_); c < outChannels; ++c) out.clear(c, 0, numSamples);
    const double dt = static_cast<double>(controlHop_) / sampleRate_;
    const bool gathering = gatheringEnergy();  // once a block: a window has the picture on
    grid_.hear(tp.ppq, tp.bpm, tp.playing, sampleRate_);
    grid_.walk(
        numSamples, controlHop_, midi, modulation(), [&](std::size_t) { step(grid_.ppqNow(), tp.bpm, tp.playing, dt); },
        [&](std::size_t pos, std::size_t m) {
            input_.analyse(pos, m);
            //  planar in, interleaved through the engine, planar out
            field_.read(in, static_cast<int>(pos), static_cast<int>(m));
            if (gathering) gatherArrived(field_.data(), static_cast<int>(m));  // before the engine has touched it
            processField(field_.data(), static_cast<int>(m));
            if (gathering)
                gatherLeaving(field_.data(), static_cast<int>(m));  // what it added, plus whatever dry it passes
            field_.write(out, static_cast<int>(pos), static_cast<int>(m));
        });

    if (gathering) publishEnergy();

    afterBlock();
    diag_.notesDropped.store(grid_.notesDropped(), std::memory_order_relaxed);
    diag_.controlPhase.store(grid_.phase(), std::memory_order_relaxed);
    diag_.outputPeak.store(out.getMagnitude(0, numSamples), std::memory_order_relaxed);
}

void FieldEffectProcessor::step(double ppq, double bpm, bool playing, double dt) noexcept {
    std::array<double, kNumFeatures> self{}, side{};
    input_.readFeatures(self, side);

    Step in{.snapshot = snapshot()};
    in.self = self;
    if (input_.sidechainActive()) in.sidechain = side;
    in.transport.playing = playing;
    in.transport.bpm = bpm;
    in.transport.ppq = ppq;
    in.dt = dt;
    in.order = engineOrder_;
    for (int slot = 0; slot < spec().regionSlots; ++slot)
        in.zeroRegionTurns[static_cast<std::size_t>(slot)] = takeRegionZeros(slot);

    controlStep(in);

    publishSources();
    /*  What another instance's window reads of this one: its sources, for a source's settings
        shown from there, and its regions as resolved, for its scene. An effect has no position -- the
        bus knows that from its product. A wait-free copy; the link tick publishes it. */
    published_.regionCount = static_cast<std::uint32_t>(spec().regionSlots);
    publishEngine(published_);  // every value as the engine has it, the sources' outputs, and when
    link().publish(published_);
}

}  // namespace bambi::host
