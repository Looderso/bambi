// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <clap-juce-extensions/clap-juce-extensions.h>
#include <cstdint>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/encode/control.hpp"
#include "bambi/encode/encoder.hpp"
#include "bambi/host/ControlGrid.h"
#include "bambi/host/Layout.h"
#include "bambi/host/PluginProcessor.h"
#include "bambi/host/Transport.h"
#include "bambi/link/directory.hpp"
#include "bambi/link/link.hpp"
#include "bambi/link/spsc.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/patch/undo.hpp"
#include "bambi/path/trajectory.hpp"

/*  bambi Encoder plugin shell.

    Everything musical is the headless core. This class is the host contract around it:
    buses, parameters, state, MIDI, and determinism against the host's timeline. Its editor
    is a diagnostic readout, not the UI.
*/
class BambiEncoderProcessor final : public bambi::host::PluginProcessor,
                                    public clap_juce_extensions::clap_juce_audio_processor_capabilities {
public:
    /// The highest order offered to hosts: a REAPER track's 128 channels hold 10th order.
    /// VST3 hosts stop at 7th regardless — that is the format's own limit.
    static constexpr int kMaxHostOrder = bambi::host::kMaxHostOrder;
    static constexpr int kDefaultOrder = 3;

    BambiEncoderProcessor();
    ~BambiEncoderProcessor() override;

    const juce::String getName() const override { return JucePlugin_Name; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;

    juce::AudioProcessorEditor* createEditor() override;

    // ---- CLAP, through the patched wrapper: ambisonic ports, one configuration per order ----
    bool isInputMain(int input) override { return input == 0; }
    int clapAmbisonicOrderForBus(bool isInput, int busIndex) override;
    uint32_t clapAudioPortsConfigCount() override;
    bool clapAudioPortsConfigGet(uint32_t index, clap_audio_ports_config* config) override;
    bool clapAudioPortsConfigSelect(clap_id configId) override;

    // ---- editing the patch, from the editor (message thread) ------------------------------
    /*  Everything the editor changes that is state rather than a host parameter -- the matrix, the
        trajectory, the envelope triggers -- goes through these. Each call is one undoable step,
        applied to the editor's copy and committed to the audio thread; a drag passes a key to
        editCoalescing, so it is one step, and ends with endEditGesture. The editor never changes
        the owned state directly -- the seam remote editing needs. */
    void edit(std::string_view name, const bambi::UndoStack::Edit& change) { document().edit(name, change); }
    void editCoalescing(std::string_view name, std::string_view key, const bambi::UndoStack::Edit& change) {
        document().editCoalescing(name, key, change);
    }
    void endEditGesture() { document().endGesture(); }
    bool undoEdit() { return document().undo(); }
    bool redoEdit() { return document().redo(); }
    bool canUndoEdit() { return document().canUndo(); }
    bool canRedoEdit() { return document().canRedo(); }

    // ---- an angle back to exactly zero, from the editor ------------------------------------------
    /*  A rate's integrated turn is engine state added to yaw, pitch or roll, not a parameter, so setting the
        parameter back to 0 leaves the path turned. This clears one axis's turn -- 0 yaw, 1 pitch, 2 roll -- at
        the audio thread's next control step. Not undoable, as a transport restart, which also clears it, is not. */
    void zeroRotation(int axis) noexcept;
    int rotationZerosPending() const noexcept { return zeroTurns_.load(std::memory_order_relaxed); }  ///< for tools

    //  ---- TargetHost: the one thing beyond the shared answers that only the encoder has -------
    void zeroPlacementTurn(int axis) override { zeroRotation(axis); }
    /*  What the two clocks have integrated, in radians. Read on the message thread from a value the
        audio thread writes, for checks and for nothing else -- the placement is drawn from the link
        bus and the region from `diagnostics().regionTurn`, not from here. */
    double placementTurnYaw() const noexcept { return control_.rotation().yawRad(); }
    double regionTurnYaw() const noexcept { return control_.regionRotation().yawRad(); }

    // ---- controlling another instance, from the editor ------------------------------------------
    /*  Message thread. What another instance last published, and edits to it: parameters as commands
        that land on its own host parameters, the patch whole into its edit cell, undo, redo and learn as
        commands. The other instance applies all of it on its own link tick. */
    bool readControls(const bambi::LinkScene::Entry& peer, bambi::LinkControls& out, std::uint32_t& generation) {
        return link().readControls(peer, out, generation);
    }
    bool sendParameter(const bambi::Uuid& target, bambi::ParamId id, float value, std::uint32_t gesture) {
        return link().sendParameter(target, id, value, gesture);
    }
    std::uint32_t sendPatchEdit(const bambi::Uuid& target, const bambi::LinkPatch& patch, std::string_view name,
                                std::string_view key, bool end) {
        return link().sendPatchEdit(target, patch, name, key, end);
    }
    bool sendControlCommand(const bambi::Uuid& target, std::uint32_t command, float value) {
        return link().sendCommand(target, command, value);
    }

    // ---- timing trace, for finding stutter --------------------------------------------------------
    /*  While on, the audio thread notes when each block arrives and the link timer when it ticks. Off, it costs
        one atomic load per block. Message thread. */
    struct TraceEvent {
        std::uint8_t kind{0};       ///< kTraceBlock or kTraceTick
        std::uint64_t us{0};        ///< linkNowMicros
        std::int64_t samples{0};    ///< a block's length
        std::int64_t timeline{-1};  ///< a block's host timeline position, -1 when unknown
    };
    static constexpr std::uint8_t kTraceBlock = 0;
    static constexpr std::uint8_t kTraceTick = 1;
    void setTracing(bool on) noexcept;
    void drainTrace(std::vector<TraceEvent>& out);

    // ---- link bus: every instance in the session, and edits between them ---------------------
    //  The audio thread only ever submits its position to a process-local LatestValue; everything
    //  that touches shared memory happens in linkTick().

    /// Scene rate: how often the position is published, and remote edits are applied.
    static constexpr int kLinkTickHz = 60;

    using LinkStatus = bambi::host::LinkStatus;

    /// Tests only: a private session directory instead of the machine's. Call before constructing.
    /// The directory belongs to the suite, not this plugin, kept as a name the checks know.
    static void setLinkDirectoryNameForTests(std::string name) {
        bambi::host::setLinkDirectoryNameForTests(std::move(name));
    }

    /*  What the host actually negotiated and sent. Written on the audio thread, read by the
        editor on the message thread; every field is atomic and nothing here is authoritative.
        The shared readout, and what only the encoder has to say beside it. */
    struct Diagnostics : bambi::host::PluginProcessor::Diagnostics {
        //  What the layout declares, beside what actually arrives in the buffer (the shared
        //  `mainInputChannels` and `sidechainChannels`). A host can differ on the two, and telling
        //  them apart is the difference between a bus we never negotiated and one we negotiated but
        //  cannot read.
        std::atomic<int> blockSize{0}, mainBusChannels{0}, sidechainBusChannels{0};
        std::atomic<int> midiEvents{0}, lastNote{-1}, resets{0};
        std::atomic<float> speed{0.0f}, azimuthDeg{0.0f}, elevationDeg{0.0f};
        std::atomic<int> blockSamples{0}, prepares{0};
    };
    const Diagnostics& diagnostics() const noexcept { return diag_; }

    /// The most recent bus-layout requests and their outcome, newest first. Message thread.
    juce::StringArray layoutLog() const;

private:
    void renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    int engineOrder() const noexcept override { return encoderOrder_; }
    void appendLayoutLog(const juce::String& entry) const;
    void resetEngine(std::int64_t timelineSample, bool fromTransport) noexcept;
    void controlStep(double ppq, double bpm, bool playing, double dt) noexcept;

    //  MIDI for the envelope triggers. A note goes to the first control step at or after its sample
    //  on the timeline; one after a block's last step waits here for the next.
    std::atomic<bool> tracing_{false};
    bambi::SpscQueue<TraceEvent, 1024> blockTrace_;  ///< audio thread -> message thread, while tracing
    std::vector<TraceEvent> tickTrace_;              ///< message thread
    std::atomic<int> zeroTurns_{0};  ///< one bit per axis whose turn to clear: message thread -> audio thread

    /// The core's control grid — the same 256 samples bambi-render steps on.
    static constexpr int kControlHop = bambi::kControlHop;
    /*  How far the host's reported timeline may sit from where this block expected it before that counts
        as a locate rather than as imprecision. Added to one block's worth of samples at the call site. */
    static constexpr std::int64_t kTimelineSlack = 64;

    /*  What the audio thread reads that a state load or an edit can change. Built whole on the
        message thread, adopted by pointer at a block boundary, never modified once posted, and
        freed back on the message thread. */
    struct EncoderSnapshot : EngineSnapshot {
        bambi::Trajectory trajectory;
        bambi::Vec3 centre{0.0, 0.0, 1.0};
        bambi::TrajectoryState shape;  ///< the path as set, its settings from their parameters
    };
    /// The snapshot the audio thread is on, as the encoder's own kind: `newSnapshot` makes no other.
    const EncoderSnapshot& current() const noexcept { return static_cast<const EncoderSnapshot&>(snapshot()); }

    //  ---- what PluginProcessor asks ---------------------------------------------------------
    std::unique_ptr<EngineSnapshot> newSnapshot(const bambi::PluginState& st) const override;
    void committing(const EngineSnapshot& next) override;
    void snapshotAdopted(const EngineSnapshot& next) noexcept override;
    void onLinkTick() override;

    /*  The path as the message thread has it, built from the committed document. It is the
        encoder's alone -- it is what a scene draws a source moving along -- and it moves with the
        document, hence the document's own lock. */
    bambi::Trajectory documentPath_;        ///< under the document's committedMutex()
    bambi::TrajectoryState documentShape_;  ///< what documentPath_ was built from; under the same lock
    /// The path as its host parameters set it now, read on the message thread.
    bambi::TrajectoryState shapeAsSet(bambi::TrajectoryState shape) const;
    bambi::FeatureBank features_, sidechainFeatures_;
    bambi::EncoderControl control_;  ///< the control step, shared with bambi-render
    bambi::Encoder encoder_;         ///< fed the input's MID, (L+R)/2: in `sum`, the whole of it
    bambi::Encoder encoderSide_;     ///< fed its side, (L-R)/2: aimed at nothing in `sum`

    std::vector<float> monoIn_, monoSidechain_;
    std::vector<float> sideIn_;  ///< (L - R) / 2 of the main input; silence from a mono bus
    std::vector<float*> outPtrs_, segmentPtrs_;
    std::vector<float> leaving_;  ///< one hop of the output, interleaved, for the energy picture: only while it is on

    double sampleRate_{48000.0};
    int encoderOrder_{-1};
    double trimDb_{0.0};
    bool sidechainActive_{false};
    bambi::host::ControlGrid grid_;  ///< where a step falls, and which step a note is: every plugin's
    bambi::host::TransportWatch transport_;

    Diagnostics diag_;

    mutable std::mutex layoutLogMutex_;
    mutable juce::StringArray layoutLog_;
    mutable juce::String lastLayoutEntry_;
    mutable int lastLayoutRepeats_{0};

    //  ---- LinkHost: what only the encoder can answer -----------------------------------------
    /// The encoder alone has a path to publish: what a scene draws a source moving along.
    void describe(bambi::LinkStatic& st) const override;
    void seedDynamic(bambi::LinkDynamic& seed) const override;
    int order() const override { return linkOrder_.load(std::memory_order_relaxed); }

    std::atomic<int> linkOrder_{kDefaultOrder};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BambiEncoderProcessor)
};
