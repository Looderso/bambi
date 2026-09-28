// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <clap-juce-extensions/clap-juce-extensions.h>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <span>
#include <vector>

#include "bambi/host/ClapPorts.h"
#include "bambi/host/ControlGrid.h"
#include "bambi/host/FieldBuffer.h"
#include "bambi/host/FieldInput.h"
#include "bambi/host/Layout.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/host/Transport.h"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/state.hpp"

/*  A field effect's processor, all of it but the engine.
 *
 *  The field comes in and the same field goes out, at the same order. What every bambi plugin is --
 *  parameters, state and its handoff, identity, the link bus and undo -- is `PluginProcessor`. What
 *  is here is what makes one a field effect: the buses and their CLAP ports, the transport and the
 *  control grid, the detectors, the planar-to-interleaved copy, the energy picture's two windows --
 *  and a plugin derives from it and answers six questions about its engine.
 *
 *  The six are virtual and are called on the audio thread, a handful of times a block: measured, a
 *  whole `processBlock` costs what it did when they were direct calls.
 */
namespace bambi::host {

class FieldEffectProcessor : public PluginProcessor,
                             public clap_juce_extensions::clap_properties,
                             public FieldEffectClapPorts<FieldEffectProcessor> {
public:
    /*  What a plugin says about itself once, when it is built. The shared keys the base reads are
     *  not here: `PluginProcessor` finds them in the manifest, once, so two of them cannot be handed
     *  over swapped and nothing is looked up by key on the audio thread. */
    struct Spec {
        Product product;
        const ParamManifest& params;
        const ModManifest& mod;
        int controlHop;                            ///< the engine's hop: the grid's step and the field buffer's size
        int regionSlots;                           ///< how many of the state's regions are this plugin's
        std::span<const FactoryPreset> factory{};  ///< handed on to `PluginProcessor::Spec`
    };

    /// Owns the diagnostics a link tick reads, so it stops the ticks first (`PluginProcessor`'s rule).
    ~FieldEffectProcessor() override { stopLink(); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;

    const Diagnostics& diagnostics() const noexcept { return diag_; }

    int engineOrder() const noexcept override { return engineOrder_; }

protected:
    /// A plugin's constructor ends with `begin(PluginState{its manifest})`, as every plugin's does.
    explicit FieldEffectProcessor(const Spec& spec);

    /// What a control step is given that is not a parameter.
    struct Step {
        std::span<const double> self;       ///< the six features of the input field
        std::span<const double> sidechain;  ///< the same of the sidechain; empty when there is none
        Transport transport;
        double dt{0.0};
        int order{0};
        std::array<int, kMaxRegions> zeroRegionTurns{};  ///< a bit per axis, per slot, taken this step
        const EngineSnapshot& snapshot;
    };

    /*  The six: what a plugin answers about its engine.
     *
     *  Nothing here tells a plugin whether the host is rendering offline: that is the one input that
     *  makes a bounce differ from playback, so a plugin that means to read it -- Reverb, for its
     *  render quality -- asks `isNonRealtime()` itself, where the decision can be seen. */
    /// Allocate for an order and a rate. The only one of the six that may allocate.
    virtual void prepareEngine(int order, double sampleRate) = 0;
    /// Forget the audio held: a start, a locate, a prepare. Audio thread.
    virtual void resetEngine() noexcept = 0;
    /// Forget what the control step holds between steps. From the transport, a region's turn goes back unless `ratesContinue`; otherwise everything does. Audio thread.
    virtual void restartControl(bool fromTransport, bool ratesContinue) noexcept = 0;
    /*  One control step: resolve the frame from the modulated parameters and hand it to the engine,
        through the plugin's resolver in `core/` -- which its render tool calls too, so the goldens
        hash this step and not a copy of it. Report each region's turn with `publishTurn`, and each
        region as resolved with `publishRegion`, for other instances' scenes. Audio thread. */
    virtual void controlStep(const Step& step) noexcept = 0;
    /// A region slot as this step resolved it, turn included: what another instance's scene draws.
    void publishRegion(int slot, const Region& region) noexcept {
        if (slot >= 0 && slot < kMaxRegions) published_.regions[slot].set(region);
    }
    /// `frames` of the field, interleaved, in place. Audio thread.
    virtual void processField(float* interleaved, int frames) noexcept = 0;
    /// After a block: whatever the plugin publishes about its engine. Audio thread.
    virtual void afterBlock() noexcept {}

    double sampleRate() const noexcept { return sampleRate_; }

private:
    void renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) final;
    void snapshotAdopted(const EngineSnapshot& next) noexcept final;
    int order() const final { return engineOrder_ < 0 ? 1 : engineOrder_; }
    void step(double ppq, double bpm, bool playing, double dt) noexcept;
    void restartAt(std::int64_t timelineSample, bool fromTransport) noexcept;

    const int controlHop_;

    FieldInput input_;  ///< W, the field's power and the sidechain, and the detectors they feed

    double sampleRate_{48000.0};
    int engineOrder_{-1};    ///< what prepareToPlay built for; -1 until it has run
    ControlGrid grid_;       ///< where a step falls, and which step a note is
    LinkDynamic published_;  ///< what this step sends other instances: its sources and regions
    TransportWatch transport_;

    /*  One hop, between the host's planar buffers and the engine. Sized at the hop, and the grid's
        segment is bounded by the same number: both are `controlHop_`. */
    FieldBuffer field_;

    /// How far the host's timeline may sit from where this block expected it before that is a locate.
    static constexpr std::int64_t kTimelineSlack = 64;

    Diagnostics diag_;
};

}  // namespace bambi::host
