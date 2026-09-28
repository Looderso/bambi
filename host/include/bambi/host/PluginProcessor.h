// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <optional>

#include "bambi/host/ControlTarget.h"
#include "bambi/host/Document.h"
#include "bambi/host/EnergyFeed.h"
#include "bambi/host/Handoff.h"
#include "bambi/host/InstanceIdentity.h"
#include "bambi/host/LinkNode.h"
#include "bambi/host/LoadMeter.h"
#include "bambi/host/Parameters.h"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/presets.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/scene/energy.hpp"
#include "bambi/scene/view.hpp"

/*  What every bambi plugin's processor is, before it is an encoder or a field effect.
 *
 *  The host's parameters from the manifest; the document, its undo and the commit that turns a state
 *  into the audio thread's snapshot, handed over lock-free; state save and load; who this instance
 *  is; the link bus, its timer and the answers it asks for; the modulation engine; a region slot's
 *  turn, cleared and published; the load meter. None of it knows what the plugin does to audio.
 *
 *  A plugin -- or `FieldEffectProcessor`, for the two that are field effects -- derives from this,
 *  says how it renders a block, and must call `begin()` at the end of its own constructor: the
 *  first snapshot is built through a virtual, and a virtual called from this constructor would be
 *  this class's and not the plugin's.
 */
namespace bambi::host {

class PluginProcessor : public juce::AudioProcessor, private LinkHost, public TargetHost, public EnergyFeed {
public:
    struct Spec {
        Product product;
        const ParamManifest& params;
        const ModManifest& mod;
        int regionSlots;  ///< how many of the state's regions are this plugin's
        /// What this plugin's preset list has after "default". Static data of the plugin's engine.
        std::span<const FactoryPreset> factory{};
    };

    ~PluginProcessor() override;

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void releaseResources() override {}
    bool hasEditor() const override { return true; }

    /// The plugin's own DSP load is measured round `renderBlock`, where it runs.
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) final;
    using AudioProcessor::processBlock;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    /*  The commit callback: every edit, undo and redo arrives here with the state to make current.
        Everything is built here, handed over with one atomic exchange, and adopted at the next
        block. Any thread but the audio thread. `snap` is what a state load wants and an edit does
        not: the engine jumps to the new patch rather than moving there. */
    void commitDocument(PluginState next, bool snap);
    /// What a save writes: the committed state, this instance's identity, the host's parameters live.
    PluginState documentState() const;

    //  ---- the patch, its history, and this instance on the bus -----------------------------
    Document& document() override { return document_; }
    /// The lock `committing` runs under, for a plugin that reads what it set there.
    std::mutex& committedMutex() const { return document_.committedMutex(); }
    LinkNode& link() { return link_; }

    //  ---- TargetHost: what a ControlTarget asks beyond LinkHost and LinkNode ----------------
    LinkHost& linkHost() override { return *this; }
    LinkNode& linkNode() override { return link_; }
    float liveSourceValue(int slot) const override {
        return diag_.sourceValues[static_cast<std::size_t>(std::clamp(slot, 0, kNumSources - 1))].load(
            std::memory_order_relaxed);
    }
    float regionTurn(int slot, int axis) const noexcept override {
        if (slot < 0 || slot >= kMaxRegions || axis < 0 || axis > 2) return 0.0f;
        return diag_.regionTurn[static_cast<std::size_t>(slot * 3 + axis)].load(std::memory_order_relaxed);
    }
    const LinkStatus& linkStatus() const { return link_.status(); }
    void updateScene(LinkScene& scene) const { link_.updateScene(scene); }

    /// What the host calls the track: the label until the user names the instance.
    void updateTrackProperties(const TrackProperties& properties) override {
        link_.setTrackName(properties.name.value_or(juce::String()));
    }

    /*  Clear a region's accumulated turn, one axis at a time: an angle set back to zero by hand
        takes the turn a rate has added with it. Message thread in, audio thread out, as one atomic
        per slot -- no allocation, no lock. */
    void zeroRegionTurn(int slot, int axis) override { zeroRegionRotation(slot, axis); }
    void zeroRegionRotation(int slot, int axis) noexcept {
        if (slot >= 0 && slot < spec_.regionSlots && axis >= 0 && axis < kTurnAxisBits)
            zeroRegionTurns_[static_cast<std::size_t>(slot)].fetch_or(1 << axis, std::memory_order_relaxed);
    }
    int regionZerosPending(int slot = 0) const noexcept {
        return slot >= 0 && slot < spec_.regionSlots
                   ? zeroRegionTurns_[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed)
                   : 0;
    }

    Identity identity() const override { return identity_.get(); }
    void renameInstance(const std::string& label) override { identity_.rename(label); }
    /// The host parameter behind a manifest id, for a control that sets it inside a gesture; also what the link node applies a remote edit through.
    juce::RangedAudioParameter* hostParameter(ParamId id) const final { return params_.object(static_cast<int>(id)); }

    /*  Learn a note for an envelope's trigger: armed on the message thread, caught on the audio
        thread from the next note-on, taken back on the message thread, where it becomes an undoable
        edit like any other -- whichever window armed it, and whether or not an editor is open. One
        envelope at a time; -1 disarms. Every plugin has envelopes, so every plugin learns. */
    void armNoteLearn(int envelope) noexcept override;
    int noteLearnArmed() const override { return learnArmed_.load(std::memory_order_relaxed); }
    /// The note caught since the last call, if there is one: its envelope, channel (1-16) and note. The tick takes it; so can a check.
    bool takeLearnedNote(int& envelope, int& channel, int& note) noexcept;

    /*  Presets: the whole sound, the same way in every plugin. What a preset is, the list and its
        order are `core`'s; here is what needs a plugin -- the host's parameters set from one, as one
        undoable step, and the files. Message thread, all of it; nothing of it is reached from the
        audio thread. The library reads the disk when it is first asked for, which is when a window
        opens. */
    PresetLibrary& presets();
    /// Read the directory again: another instance may have written. When the browser opens.
    void refreshPresets() { presets().scan(); }
    PresetRef currentPreset() const;
    /// Whether the patch has moved from the preset it names. A preset whose file is gone counts as moved.
    bool presetModified();
    bool loadPreset(const PresetRef& ref);
    /// The preset `delta` on from the current one in list order, loaded.
    bool stepPreset(int delta);
    /// Write the patch as it is now as a user preset, over one of the same name, and carry its name.
    bool savePreset(const PresetRef& ref);
    bool renamePreset(const PresetRef& from, const PresetRef& to);
    /// To the Trash. The patch stays as it is.
    bool removePreset(const PresetRef& ref);
    /*  This plugin's own directory: `BAMBI_PRESET_DIR/<Plugin>` when that is set -- a check's, whose
        files are deleted outright -- or the user's, whose files go to the Trash. */
    static std::filesystem::path presetRoot(Product p);
    /// Sets `BAMBI_PRESET_DIR` for this process, or clears it when `dir` is empty. For checks and tools.
    static void usePresetDirectory(const std::filesystem::path& dir);

    /*  One tick of the bus, as the timer drives it; public so a check can drive it without waiting
        on a message loop. Freeing what the audio thread retired is the plugin's, because the handoff
        is -- posting drains it too, but a session where nothing commits for a while should still
        give the memory back. */
    void linkTick();
    /// Tell the audio thread whether someone elsewhere is looking, and put a new pair on the bus if so.
    void shareEnergy();
    /// The user did something here: a new instance elsewhere should join this session.
    void noteUserActivity() { link_.noteActivity(); }

    /*  How the window was left. The view is kept for as long as the plugin is loaded, so a window
        closed and opened again comes back to it; it is not saved, and a project opens on the
        defaults. The energy picture is not here: it is off whenever a window opens. Message thread. */
    struct ViewMemory {
        bool kept{false};  ///< a window has closed since this instance was made
        Camera camera;
        ViewPreset preset{ViewPreset::Free};
        bool regionsAlways{true};
        bool equirectFull{false};
    };
    ViewMemory& viewMemory() { return viewMemory_; }
    /*  The window's size, as a multiple of its design size: saved with the session, as the identity is
        (`PluginState::windowScale`). Atomic because a host may save from any thread. */
    float windowScale() const { return windowScale_.load(std::memory_order_relaxed); }
    void setWindowScale(float scale) {
        windowScale_.store(std::clamp(scale, kWindowScaleMin, kWindowScaleMax), std::memory_order_relaxed);
    }

    /*  What the audio thread publishes about itself that every plugin has. Written there, read by
        the editor on the message thread; every field is atomic and nothing here is authoritative.
        A plugin with more to say derives its own from this and hands it to the constructor. */
    struct Diagnostics {
        std::atomic<int> order{-1};
        std::atomic<int> mainInputChannels{0}, outputChannels{0}, sidechainChannels{0};
        std::atomic<bool> playing{false}, transportKnown{false}, nonRealtime{false};
        std::atomic<float> inputPeak{0.0f}, outputPeak{0.0f}, sidechainPeak{0.0f};
        std::atomic<int> layoutMismatches{0};
        std::atomic<double> bpm{120.0}, sampleRate{48000.0};
        std::atomic<std::int64_t> timeInSamples{-1};
        std::atomic<std::uint64_t> stateSequence{0};      ///< the patch the audio thread is on
        std::atomic<std::uint64_t> adoptionsDeferred{0};  ///< a mirror of the handoff's total, not a counter of its own
        /*  Samples since the last control step. It exists so a check can see that the grid is pinned
            to the host's timeline and not to wherever processing began, and that is otherwise
            invisible from outside: two renders that both begin at a reset agree however it is
            pinned. */
        std::atomic<int> controlPhase{0};
        std::atomic<int> notesDropped{0};  ///< notes the grid's carry-over could not hold, ever
        std::array<std::atomic<float>, kNumSources>
            sourceValues{};  ///< each source after its amount, last control step
        /*  What the rates have turned each region by, in radians: yaw, pitch, roll, a slot after
            another. The editor draws the region the audio thread is applying, and a rate-driven one
            is not where its parameters alone say it is. */
        std::array<std::atomic<float>, kMaxRegions * 3> regionTurn{};
    };
    /// The shared readout, whatever the plugin's own adds to it: what every settings page shows of the host.
    const Diagnostics& readout() const noexcept { return diag_; }
    float dspLoad() const noexcept { return load_.load(); }
    float dspPeak() const noexcept { return load_.peak(); }

    //  ---- EnergyFeed: the energy picture's summary ------------------------------------------
    bool takeCovariances(std::vector<float>& added, std::vector<float>& arrived) final;
    void wantEnergy(bool wanted) noexcept final { energyWanted_.store(wanted, std::memory_order_relaxed); }
    bool energyWanted() const noexcept { return energyWanted_.load(std::memory_order_relaxed); }
    /// Another instance's window is looking at this one's energy, as of the last link tick.
    bool energyWantedElsewhere() const noexcept { return remoteWanted_.load(std::memory_order_relaxed); }

    static int liveEngineSnapshots() { return SnapshotCensus::live(); }
    static int engineSnapshotsFreedOnAudioThread() { return SnapshotCensus::freedOnAudioThread(); }

    /*  What the audio thread adopts as one pointer: everything about the patch that is not a host
        parameter. Every region the state has and the render quality ride along whether a plugin
        reads them or not -- the protocol does not care how small its cargo is. A plugin with more to
        carry -- the encoder's built trajectory -- derives from it (`newSnapshot`). */
    struct EngineSnapshot : SnapshotCensus {
        virtual ~EngineSnapshot() = default;
        ModulationPatch patch;
        std::array<RegionEntry, kMaxRegions> regions{};
        RenderQuality renderQuality{RenderQuality::Realistic};
        RoomState room;  ///< Reverb's shape rides here
        std::uint64_t sequence{0};
        bool snap{true};
    };

protected:
    /// `diagnostics` is the plugin's own, or one derived from it; it is only bound here, so it may be a member of the class being constructed.
    PluginProcessor(const Spec& spec, const BusesProperties& buses, Diagnostics& diagnostics);

    /*  The last line of the most derived constructor. Builds the first snapshot from `initial` with
        the host's parameters in it, starts the document, opens the link directory and starts the
        bus's timer. */
    void begin(PluginState initial);

    //  ---- what a plugin answers ---------------------------------------------------------------
    /// One block. Audio thread.
    virtual void renderBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) = 0;
    /*  A snapshot to fill, with whatever this plugin carries beyond the shared cargo already built
        into it. May allocate: it is never called on the audio thread. */
    virtual std::unique_ptr<EngineSnapshot> newSnapshot(const PluginState&) const {
        return std::make_unique<EngineSnapshot>();
    }
    /// A snapshot is about to be posted, under the document's lock: for what must change together with the committed state -- the encoder's message-thread copy of its path.
    virtual void committing(const EngineSnapshot&) {}
    /*  The audio thread has just adopted `next`; the modulation patch is already in use. What is
        left is the plugin's: force a control step, ask the engine to snap. Audio thread. */
    virtual void snapshotAdopted(const EngineSnapshot& next) noexcept = 0;
    /// Before the bus's own tick: what a plugin takes back from its audio thread. Message thread.
    virtual void onLinkTick() {}

    /*  The rule for a derived class: its members are destroyed before this class's timer and link
        node stop, and a tick reads the `Diagnostics` a derived class owns. So every class that owns
        the diagnostics, or overrides a link callback -- `describe`, `seedDynamic`, `applyCommand`,
        `onLinkTick` -- to touch a member of its own, calls `stopLink()` first thing in its
        destructor. JUCE destroys a processor on the message thread, where no tick can interleave;
        this is for the host that does not. */
    void stopLink() { linkTimer_.stopTimer(); }

    //  ---- what rendering a block uses ---------------------------------------------------------
    /// A state posted since the last block takes effect here, at the boundary. Audio thread.
    void adoptPendingSnapshot() noexcept;
    const EngineSnapshot& snapshot() const noexcept {
        jassert(handoff_.current() != nullptr);  // a constructor that did not end with begin()
        return *handoff_.current();
    }
    /// One lock-free copy of the host's parameters, a block. Audio thread.
    void intakeParameters() noexcept { params_.intake(); }
    const std::array<float, kMaxParams>& parameterValues() const noexcept { return params_.values; }
    /// A shared key's value this block by its position, or `fallback` where the list has no such key.
    float sharedValue(int at, float fallback) const noexcept {
        return at >= 0 && at < spec_.params.size() ? params_.values[static_cast<std::size_t>(at)] : fallback;
    }
    ModulationEngine& modulation() noexcept { return mod_; }
    /*  What every window reads of the engine this step, into the dynamic record: every parameter as
        it has it, every source's output, and when. One call, from each plugin's control step. Audio
        thread. */
    void publishEngine(LinkDynamic& into) const noexcept {
        const int n = std::min(spec_.params.size(), kMaxParams);
        into.liveCount = static_cast<std::uint32_t>(n);
        for (int i = 0; i < n; ++i) into.live[i] = static_cast<float>(mod_.destination(static_cast<ParamId>(i)));
        for (int i = 0; i < kNumSources; ++i) into.sources[i] = static_cast<float>(mod_.sourceValue(i));
        into.sampledUs = linkNowMicros();
    }
    /// The bits of a slot's turn to clear, taken: a bit per axis. Audio thread.
    int takeRegionZeros(int slot) noexcept {
        return slot >= 0 && slot < spec_.regionSlots
                   ? zeroRegionTurns_[static_cast<std::size_t>(slot)].exchange(0, std::memory_order_relaxed)
                   : 0;
    }
    /// Give the armed envelope this block's first note-on, if one is armed. Audio thread, once a block.
    void catchLearnedNote(const juce::MidiBuffer& midi) noexcept;
    /*  The energy picture's summary. `prepareEnergy` allocates, once; the rest is the audio thread's.
        A block asks `gatheringEnergy()` first and gathers only if so: a window has the picture on,
        and the host is not rendering offline -- nobody watches a bounce, and what is gathered never
        reaches the audio, so a bounce is the same either way. `withArrived` is a field effect's: an
        encoder has nothing arriving. Both take interleaved frames. */
    void prepareEnergy(int order, double sampleRate, bool withArrived);
    void resetEnergy() noexcept;
    /*  Asked once a block. When it turns true after having been false, the windows start empty: a
        window half-filled before the picture was switched off would otherwise be finished with
        audio from minutes later, and the first picture back would be of neither. */
    bool gatheringEnergy() noexcept {
        const bool gathering = (energyWanted() || remoteWanted_.load(std::memory_order_relaxed)) && !isNonRealtime();
        if (gathering && !wasGathering_) resetEnergy();
        wasGathering_ = gathering;
        return gathering;
    }
    void gatherArrived(const float* interleaved, int frames) noexcept { arrivedWindow_.add(interleaved, frames); }
    void gatherLeaving(const float* interleaved, int frames) noexcept { addedWindow_.add(interleaved, frames); }
    /// A window's worth has been gathered: publish it. A few hundred floats copied; nothing allocates.
    void publishEnergy() noexcept;
    void publishTurn(int slot, const RotationClock& turn) noexcept;
    /// Every source's value after its amount, as the last control step left it. Audio thread.
    void publishSources() noexcept;
    void setLoadRate(double sampleRate) noexcept { loadRate_ = sampleRate; }

    const Spec& spec() const noexcept { return spec_; }
    /// The three keys every plugin's list carries, found once in the constructor.
    int ratesRetriggerAt() const noexcept { return ratesRetriggerAt_; }
    int levelReleaseAt() const noexcept { return levelReleaseAt_; }
    int scLevelReleaseAt() const noexcept { return scLevelReleaseAt_; }

    //  ---- LinkHost: a plugin may say more about itself than the node fills in -----------------
    void describe(LinkStatic&) const override {}
    void seedDynamic(LinkDynamic&) const override {}
    int order() const override = 0;  ///< what it renders at, for the scene's readout: the plugin's to say

private:
    std::unique_ptr<EngineSnapshot> buildSnapshot(const PluginState& st, bool snap) const;
    /// The host's parameters set from a state's, the ones that differ. Any thread but the audio thread.
    void setHostParameters(const std::array<float, kMaxParams>& values);

    Product product() const final { return spec_.product; }
    const ParamManifest& manifest() const final { return spec_.params; }
    const ModManifest& modManifest() const final { return spec_.mod; }
    float parameterValue(int at) const final { return params_.live(at); }
    std::int8_t learning() const final { return static_cast<std::int8_t>(learnArmed_.load(std::memory_order_relaxed)); }
    void adoptIdentity(const Uuid& session, const Uuid& instance) final { identity_.adopt(session, instance); }

    const Spec spec_;
    int ratesRetriggerAt_{kNoParam}, levelReleaseAt_{kNoParam}, scLevelReleaseAt_{kNoParam};

    Handoff<EngineSnapshot> handoff_;

    juce::AudioProcessorValueTreeState apvts_;
    std::array<std::atomic<int>, kMaxRegions> zeroRegionTurns_{};  ///< a bit per axis, per slot
    std::atomic<int> learnArmed_{-1};  ///< the envelope waiting for a note: message thread -> audio thread
    std::atomic<int> learned_{-1};     ///< envelope << 16 | channel << 8 | note: audio thread -> message thread
    Parameters params_;
    ViewMemory viewMemory_;
    std::atomic<float> windowScale_{1.0f};

    /*  Before `link_`, which reads it on its first tick. */
    InstanceIdentity identity_;

    Document document_;
    LinkNode link_{*this};

    struct LinkTimer final : juce::Timer {
        explicit LinkTimer(PluginProcessor& p) : owner(p) {}
        void timerCallback() override { owner.linkTick(); }
        PluginProcessor& owner;
    };

    /*  The two windows and the cell they publish into. The cell is written by the audio thread under
        no lock and read by the editor: a torn read costs one frame of a 20 Hz picture, and a lock on
        the audio thread costs a dropout. `covGeneration_` is what says a pair is new. */
    CovarianceWindow addedWindow_, arrivedWindow_;
    std::vector<float> addedCell_, arrivedCell_;
    std::atomic<std::uint32_t> covGeneration_{0};
    std::uint32_t covTaken_{0};
    std::atomic<bool> energyWanted_{false};
    /*  Another instance's window is looking at this one: gathered for it, and each new pair is put
        on the bus by `linkTick`, cut to kLinkEnergyOrder. Message thread writes both. */
    std::atomic<bool> remoteWanted_{false};
    std::uint32_t covBusTaken_{0};
    LinkEnergy busEnergy_{};
    std::atomic<int> energyOrder_{0};  ///< what the windows are built at
    bool withArrived_{false};
    bool wasGathering_{false};  ///< audio thread only

    PluginState fresh_;                       ///< what `begin` was given: what "default" loads
    std::unique_ptr<PresetLibrary> presets_;  ///< message thread; built when first asked for

    ModulationEngine mod_;
    LoadMeter load_;  ///< this plugin's own share of each block's budget, for the footer
    double loadRate_{48000.0};
    Diagnostics& diag_;

    //  Last, so it is destroyed -- and stops calling linkTick -- before anything linkTick uses.
    LinkTimer linkTimer_{*this};
};

}  // namespace bambi::host
