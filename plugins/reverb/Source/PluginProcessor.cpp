// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "bambi/reverb/presets.hpp"

BambiReverbProcessor::BambiReverbProcessor()
    : bambi::host::FieldEffectProcessor({.product = bambi::Product::Reverb,
                                         .params = bambi::reverbParams(),
                                         .mod = bambi::reverbMod(),
                                         .controlHop = bambi::kReverbHop,
                                         .regionSlots = 2,  // two region slots: the send and the return
                                         .factory = bambi::reverbFactoryPresets()}) {
    begin(bambi::PluginState{bambi::reverbParams()});
}

double BambiReverbProcessor::getTailLengthSeconds() const {
    /*  The longest decay the room offers, plus the pre-delay that can sit in front of it. Taken from
        the parameter RANGES and not from the live values: a host reads this once, outside any block,
        and a tail cut off is worse than one that runs on. */
    const auto& m = bambi::reverbParams();
    const int decay = m.byKey("room.decay");
    const int pre = m.byKey("output.pre_delay");
    const double longest = decay == bambi::kNoParam ? 10.0 : static_cast<double>(m[decay].max);
    const double delay = pre == bambi::kNoParam ? 0.2 : static_cast<double>(m[pre].max) / 1000.0;
    return longest + delay;
}

bambi::RenderQuality BambiReverbProcessor::renderQuality() const {
    return const_cast<BambiReverbProcessor*>(this)->document().committed().renderQuality;
}

void BambiReverbProcessor::setRenderQuality(bambi::RenderQuality q) {
    /*  An undoable document edit, so it reaches the audio thread the way every other patch change
        does -- and so another window can undo it, which is the whole point of it being state
        rather than a member. */
    if (document().editing().renderQuality == q) return;
    document().edit("render quality", [q](bambi::PluginState& s) { s.renderQuality = q; });
}

void BambiReverbProcessor::restartControl(bool fromTransport, bool ratesContinue) noexcept {
    /*  What Reverb holds across steps is its two regions' accumulated turns. They go back, so a
        render repeats, unless the user said continue; the tail itself is the engine's. */
    if (fromTransport)
        control_.restartTransport(ratesContinue);
    else
        control_.reset();
}

void BambiReverbProcessor::controlStep(const Step& step) noexcept {
    bambi::ReverbControlInput in;
    in.self = step.self;
    in.sidechain = step.sidechain;
    in.transport = step.transport;
    in.dt = step.dt;
    //  The one thing that may make a bounce differ from playback, asked for here and nowhere shared:
    //  it is what `renderQuality` reads, by the user's choice.
    in.offline = isNonRealtime();
    in.renderQuality = step.snapshot.renderQuality;
    in.roomShape = static_cast<bambi::RoomShape>(std::clamp(step.snapshot.room.shape, 0, 2));
    /*  A room change is switched to whole: its size glides at 250 ms and its decay at 40, so a glide
        from one room to another would otherwise pass through settings no room has, with the dry up to
        27 dB above the input. The two networks still crossfade; only the settings no longer travel. */
    if (!(step.snapshot.room == room_)) {
        room_ = step.snapshot.room;
        modulation().settle();
    }
    in.sendShape = step.snapshot.regions[0].shape;
    in.returnShape = step.snapshot.regions[1].shape;
    in.zeroRegionTurns[0] = step.zeroRegionTurns[0];
    in.zeroRegionTurns[1] = step.zeroRegionTurns[1];

    //  What happens in a step is bambi::ReverbResolver's, which tools/reverb-render calls too: the
    //  goldens hash this step and not a copy of it.
    const auto frame = control_.step(modulation(), bambi::reverbParams(), parameterValues(), in, sampleRate());
    engine_.set(frame);
    publishRegion(0, frame.send);  // for other instances' scenes
    publishRegion(1, frame.returnRegion);
    publishTurn(0, control_.sendTurn());
    publishTurn(1, control_.returnTurn());
    tailLines_.store(engine_.lines(), std::memory_order_relaxed);
    rtMid_.store(static_cast<float>(engine_.room().rtMid), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* BambiReverbProcessor::createEditor() { return new BambiReverbEditor(*this); }
