// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"

#include "PluginEditor.h"

BambiEchoProcessor::BambiEchoProcessor()
    : bambi::host::FieldEffectProcessor({.product = bambi::Product::Echo,
                                         .params = bambi::echoParams(),
                                         .mod = bambi::echoMod(),
                                         .controlHop = bambi::kEchoHop,
                                         .regionSlots = 1})  // one region slot: the send
{
    begin(bambi::PluginState{bambi::echoParams()});
}

double BambiEchoProcessor::getTailLengthSeconds() const {
    /*  The longest tap the engine's rings hold, times the repeats a feedback at the top of its range
        keeps audible. Rounded up rather than derived from the live parameters: a host reads this
        once, outside any block, and a tail that is cut off is worse than one that runs on. */
    return bambi::kMaxStoredSeconds * 8.0;
}

void BambiEchoProcessor::restartControl(bool fromTransport, bool ratesContinue) noexcept {
    /*  What Echo holds across steps is its taps' periods and its region's turn. The periods always
        go back, so a render repeats; the turn goes back too unless the user said continue. */
    if (fromTransport)
        control_.restartTransport(ratesContinue);
    else
        control_.reset();
}

void BambiEchoProcessor::controlStep(const Step& step) noexcept {
    bambi::EchoControlInput in;
    in.self = step.self;
    in.sidechain = step.sidechain;
    in.transport = step.transport;
    in.dt = step.dt;
    in.order = step.order;
    in.sendShape = step.snapshot.regions[0].shape;
    in.zeroRegionTurns = step.zeroRegionTurns[0];

    //  What happens in a step is bambi::EchoResolver's, which bambi-echo-render calls too: the
    //  goldens hash this step and not a copy of it.
    const auto frame = control_.step(modulation(), bambi::echoParams(), parameterValues(), in, sampleRate());
    engine_.setFrame(frame);
    publishRegion(0, frame.send);  // for other instances' scenes
    publishTurn(0, control_.sendTurn());
}

juce::AudioProcessorEditor* BambiEchoProcessor::createEditor() { return new BambiEchoEditor(*this); }
