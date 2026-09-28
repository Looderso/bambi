// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "bambi/host/FieldEffectProcessor.h"
#include "bambi/reverb/control.hpp"
#include "bambi/reverb/engine.hpp"
#include "bambi/reverb/params.hpp"

/*  bambi Reverb: the suite's third plugin.
 *
 *  A field effect, as Echo is, and everything the two share is `host::FieldEffectProcessor`.
 *  What is left is what a plugin owns -- its engine and its parameter set -- and the two things
 *  Reverb has that Echo does not: a return region beside the send, and a render quality a bounce
 *  follows.
 *
 *  Verified headless by `bambi-reverb-check`.
 */
class BambiReverbProcessor final : public bambi::host::FieldEffectProcessor {
public:
    BambiReverbProcessor();

    const juce::String getName() const override { return JucePlugin_Name; }
    double getTailLengthSeconds() const override;
    juce::AudioProcessorEditor* createEditor() override;

    /*  What a bounce should do. State, not a host parameter -- nobody rides it in a mix --
        so setting it is a document edit and it is saved and loaded with the rest. */
    bambi::RenderQuality renderQuality() const;
    void setRenderQuality(bambi::RenderQuality q);

    /// What the quality switch settled on, and the room the engine built: for the readout.
    int tailLines() const noexcept { return tailLines_.load(std::memory_order_relaxed); }
    float rtMid() const noexcept { return rtMid_.load(std::memory_order_relaxed); }

private:
    void prepareEngine(int order, double sampleRate) override { engine_.prepare(order, sampleRate); }
    void resetEngine() noexcept override { engine_.reset(); }
    void restartControl(bool fromTransport, bool ratesContinue) noexcept override;
    void controlStep(const Step& step) noexcept override;
    void processField(float* interleaved, int frames) noexcept override {
        engine_.process(interleaved, interleaved, frames);
    }

    bambi::ReverbResolver control_;  ///< the control step, shared with bambi-reverb-render
    bambi::ReverbEngine engine_;
    std::atomic<int> tailLines_{0};
    std::atomic<float> rtMid_{0.0f};
    bambi::RoomState room_{};  ///< the room the last step played: a different one is switched to whole

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiReverbProcessor)
};
