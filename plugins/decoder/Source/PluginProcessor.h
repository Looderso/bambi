// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <clap-juce-extensions/clap-juce-extensions.h>
#include <cstdint>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "HelperLink.h"
#include "bambi/decode/decoder.hpp"
#include "bambi/decode/headmodel.hpp"
#include "bambi/decode/headrotation.hpp"
#include "bambi/decode/orientation.hpp"
#include "bambi/decode/params.hpp"
#include "bambi/decode/speakers.hpp"
#include "bambi/decode/virtualarray.hpp"
#include "bambi/host/FieldBuffer.h"
#include "bambi/host/Layout.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/link/latest.hpp"

/*  bambi Decoder: the suite's fourth plugin, a monitor rather than an effect.
 *
 *  An ambisonic field comes in at the order that fits the input bus; the output bus is as wide as
 *  the host gives and is never renegotiated, and every output is a set of channels on it: headphones
 *  on a pair, a loudspeaker layout on its file's channel numbers. Two outputs play at once whenever
 *  their channels do not collide; one needing more channels than the bus has does not play. The head
 *  turns the field for headphones -- from a tracker over OSC, or from yaw, pitch and roll as
 *  automated -- and turns nothing for the loudspeakers, since the listener is really in the room.
 *
 *  Everything that is not a host parameter -- what plays and where, the layout, the listener's two
 *  sliders, the weighting, the tracker's setup -- is state (`DecoderState`), edited as a document and
 *  adopted whole. The filters and the decoder are designed on commit, here on the message thread, as
 *  Reverb builds its room, so a state load is heard whole and a bounce hears the state, never a
 *  worker's timing. The audio thread swaps finished sets in whole (`core/decode`).
 *
 *  Verified headless by `bambi-decoder-check`.
 */
class BambiDecoderProcessor final : public bambi::host::PluginProcessor,
                                    public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                                    private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback> {
public:
    static constexpr int kMaxOutputs = 64;  ///< the widest bus a layout may use
    static constexpr int kPartition = 64;   ///< the binaural convolution's block, and the latency reported
    static constexpr int kMaxHostOrder = bambi::host::kMaxHostOrder;

    BambiDecoderProcessor();
    ~BambiDecoderProcessor() override;

    const juce::String getName() const override { return JucePlugin_Name; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    juce::AudioProcessorEditor* createEditor() override;

    int clapAmbisonicOrderForBus(bool isInput, int busIndex) override;
    uint32_t clapAudioPortsConfigCount() override;
    bool clapAudioPortsConfigGet(uint32_t index, clap_audio_ports_config* config) override;
    bool clapAudioPortsConfigSelect(clap_id configId) override;

    //  ---- the listening situation: state, edited as a document ---------------------------------
    /// Message thread: one undoable edit of the decoder's state, committed to the audio thread.
    void editDecoder(std::string_view name, const std::function<void(bambi::DecoderState&)>& change);
    enum class Output { Headphones, Speakers };
    /// Light or stop an output. Lighting one whose channels collide with another's stops that one and says so in
    /// `notice()`; one needing more channels than the bus has does not light and says how many it needs.
    void playOutput(Output which, bool on);
    /// The channels an output would write, counted from 1, as the state has it.
    std::vector<int> channelsOf(Output which) const;
    /// The current state, copied: for the editor.
    bambi::DecoderState decoderState() const;
    /// Take the yaw off the tracker's orientation at the next block; pitch and roll stay. Any thread.
    void recentre() noexcept { recentre_.store(true, std::memory_order_relaxed); }

    //  ---- what the window reads -----------------------------------------------------------------
    struct Diagnostics : bambi::host::PluginProcessor::Diagnostics {
        std::atomic<int> blockSize{0};
        std::atomic<int> speakerOutputs{0};  ///< channels the layout's design spans; 0 without a layout
        /// The block's peak on each output channel, linear, the first kMaxChannels of the bus; 0 where nothing plays.
        std::array<std::atomic<float>, bambi::kMaxChannels> channelPeak{};
        std::atomic<bool> headphonesHeard{false}, speakersHeard{false};
        std::atomic<int> trackerMessages{0};
        std::atomic<std::int64_t> trackerLastMs{0};  ///< when the last orientation arrived, juce::Time millis
        std::atomic<float> headYaw{0.0f}, headPitch{0.0f}, headRoll{0.0f};  ///< what turns the field now, degrees
    };
    const Diagnostics& diagnostics() const noexcept { return diag_; }
    int engineOrder() const noexcept override { return engineOrder_; }
    bool modelLoaded() const noexcept { return modelLoaded_; }
    const bambi::HeadModel& headModel() const noexcept {
        return chosenHead_ == 2 ? fileModel_ : models_[static_cast<std::size_t>(chosenHead_)];
    }
    /// Why the layout could not be designed, or empty. Message thread.
    juce::String designError() const;
    /// The layout's design as last built, for the page and the scene. Message thread.
    bambi::SpeakerDesign speakerDesign() const;
    /// What the last `playOutput` had to say, or empty.
    juce::String notice() const;
    /// The helper's last word about itself: the permission, the headphones, the count. Empty before it speaks.
    juce::String helperStatus() const;
    /// Whether this build and this bundle can read AirPods: macOS, and the helper nested in the bundle.
    bool helperAvailable() const { return HelperLink::available() && !HelperLink::bundledHelperPath().empty(); }
    /*  The look-and-nod calibration of how the AirPods sit: captureLevel() while the head looks straight ahead and
        level, captureNod() while it looks down from there. The second computes the axes, stores them as state and
        recentres. Message thread. False when nothing has arrived from the device yet, or the nod was no nod. */
    bool captureLevel();
    bool captureNod();
    bool levelCaptured() const { return levelCaptured_; }
    void forgetCalibration();  ///< back to Apple's documented frame
    juce::StringArray layoutLog() const;

    /// The sliders the binaural filters were last built with, for a check.
    std::pair<double, double> builtSliders() const { return {builtShift_, builtSize_}; }
    int builtHead() const {
        return chosenHead_;
    }  ///< 0 the model, 1 the KU100, 2 a file: what the filters are built from
    /*  Message thread: a SOFA file read, fitted into a head and put into the state as a head file's text, so the
        session opens without the file; the head plays at the next commit. Seconds for a dense set. False, with
        `error`, when the file is not a SOFA file of two ears. */
    bool loadSofaFile(const juce::File& file, juce::String* error = nullptr);
    const juce::String& sofaNote() const { return sofaNote_; }

private:
    struct DecoderSnapshot : EngineSnapshot {
        bambi::DecoderState decoder;
    };
    const DecoderSnapshot& current() const noexcept { return static_cast<const DecoderSnapshot&>(snapshot()); }

    void renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    std::unique_ptr<EngineSnapshot> newSnapshot(const bambi::PluginState& st) const override;
    void committing(const EngineSnapshot& next) override;
    void snapshotAdopted(const EngineSnapshot& next) noexcept override;
    void describe(bambi::LinkStatic& st) const override;
    int order() const override { return engineOrder_ < 0 ? 1 : engineOrder_; }

    /// Designs what `d` asks for that is not built yet: the filters for the sliders, the layout's decoder, the
    /// array on headphones. Message thread, under `workMutex_`. Allocates.
    void build(const bambi::DecoderState& d);
    /// Parses the state's file head when its text changed; then which head the state asks for and can have.
    int headIndex(const bambi::DecoderState& d);
    void connectTracker(const bambi::DecoderState& d);
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void appendLayoutLog(const juce::String& entry) const;

    std::array<bambi::HeadModel, 2> models_;  ///< the model, and the KU100 as measured
    bool modelLoaded_{false};
    bambi::HeadModel fileModel_;                       ///< a SOFA file's head, parsed from the state's text
    std::shared_ptr<const std::string> fileHeadText_;  ///< the text fileModel_ was parsed from
    bool fileLoaded_{false};
    int chosenHead_{0};      ///< 0 the model, 1 the KU100, 2 the file: what the chain plays; message thread
    juce::String sofaNote_;  ///< what the last file load said: the set and how close the fit came

    //  The engines. Prepared in prepareToPlay, fed on commit, read by the audio thread.
    bambi::BinauralDecoder binaural_;
    bambi::SpeakerDecoder speakers_;
    bambi::VirtualArray virtual_;
    bambi::HeadRotation turn_;
    std::mutex workMutex_;  ///< prepare and build: never the audio thread
    double sampleRate_{48000.0};
    int engineOrder_{-1};  ///< what prepareToPlay built for; -1 until it has run
    int maxBlock_{0};
    double builtShift_{0.0}, builtSize_{1.0};
    bool headBuilt_{false};
    std::vector<bambi::Loudspeaker> builtLayout_;
    int builtWeighting_{-1}, builtOrder_{-1};
    bool builtDistance_{true};

    bambi::host::FieldBuffer field_;                    ///< the input, interleaved
    std::vector<float> turned_, stereo_, speakersOut_;  ///< scratch, sized in prepareToPlay
    std::vector<float> gainRamp_;                       ///< the level per sample of the block
    float gainNow_{1.0f}, gainCoefficient_{1.0f};

    //  The tracker. The OSC callback runs on the message thread and is the handoff's one writer; the audio
    //  thread reads it and is the rotation stage's one writer, choosing between the tracker and the parameters.
    juce::OSCReceiver osc_;
    int oscPort_{0};
    bool oscConnected_{false};
    HelperLink helper_;          ///< the AirPods helper, launched while the tracker source is AirPods
    juce::String helperStatus_;  ///< under trackerMutex_
    mutable std::mutex trackerMutex_;
    bambi::OrientationRead trackerRead_;
    bambi::LatestValue<bambi::Mat3> tracker_;
    bambi::Mat3 rawNow_{bambi::kIdentity3};  ///< the device's own orientation, last received; under trackerMutex_
    bool rawSeen_{false};
    bambi::Mat3 levelPose_{bambi::kIdentity3};  ///< the calibration's first capture; message thread
    bool levelCaptured_{false};
    bambi::Mat3 trackerNow_{bambi::kIdentity3};  ///< audio thread
    bool trackerSeen_{false};
    double yaw0_{0.0};
    std::atomic<bool> recentre_{false};

    Diagnostics diag_;
    mutable std::mutex logMutex_;
    mutable juce::StringArray layoutLog_;  ///< isBusesLayoutSupported is const and writes it
    juce::String notice_;
};
