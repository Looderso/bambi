// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "bambi/echo/control.hpp"
#include "bambi/echo/engine.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/host/FieldEffectProcessor.h"

/*  bambi Echo: the suite's second plugin.
 *
 *  A field effect -- the field comes in and the same field goes out -- and everything true of
 *  every field effect is `host::FieldEffectProcessor`: the buses, the parameters, the state and
 *  its handoff, identity, the link bus and undo, the transport and the control grid, the
 *  detectors, the energy picture. What is here is what a plugin owns on its own: its engine and
 *  its parameter set.
 *
 *  Verified headless by `bambi-echo-check`.
 */
class BambiEchoProcessor final : public bambi::host::FieldEffectProcessor {
public:
    BambiEchoProcessor();

    const juce::String getName() const override { return JucePlugin_Name; }
    double getTailLengthSeconds() const override;
    juce::AudioProcessorEditor* createEditor() override;

    /// Frames a loop was held at +80 dBFS: what says a feedback setting ran away.
    long long clamped() const noexcept { return clamped_.load(std::memory_order_relaxed); }

private:
    void prepareEngine(int order, double sampleRate) override { engine_.prepare(order, sampleRate); }
    void resetEngine() noexcept override { engine_.reset(); }
    void restartControl(bool fromTransport, bool ratesContinue) noexcept override;
    void controlStep(const Step& step) noexcept override;
    void processField(float* interleaved, int frames) noexcept override {
        engine_.process(interleaved, interleaved, frames);
    }
    void afterBlock() noexcept override { clamped_.store(engine_.clamped(), std::memory_order_relaxed); }

    bambi::EchoResolver control_;  ///< the control step, shared with bambi-echo-render
    bambi::EchoEngine engine_;
    std::atomic<long long> clamped_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiEchoProcessor)
};
