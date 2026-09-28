// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/PluginProcessor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "bambi/mod/sources.hpp"

namespace bambi::host {

PluginProcessor::PluginProcessor(const Spec& spec, const BusesProperties& buses, Diagnostics& diagnostics)
    : AudioProcessor(buses),
      spec_(spec),
      apvts_(*this, nullptr, "bambi", parameterLayout(spec.params)),
      document_(
          spec.params,
          [this](PluginState next, bool whole) {
              //  the parameters first: the block that snaps to the new patch reads them
              if (whole) setHostParameters(next.params);
              commitDocument(std::move(next), whole);
          },
          [this](PluginState& st) {
              for (int i = 0; i < spec_.params.size(); ++i) st.params[static_cast<std::size_t>(i)] = params_.live(i);
          }),
      diag_(diagnostics) {
    //  by key, here and once: on the audio thread they are positions
    ratesRetriggerAt_ = spec_.params.byKey("rates.retrigger");
    levelReleaseAt_ = spec_.params.byKey("level.release");
    scLevelReleaseAt_ = spec_.params.byKey("sc_level.release");
    jassert(ratesRetriggerAt_ != kNoParam && levelReleaseAt_ != kNoParam && scLevelReleaseAt_ != kNoParam);

    /*  Host parameters come straight from the manifest: one registration per descriptor, keyed by
        the same stable key the state format uses. Adding a parameter is a manifest edit, never a
        plugin edit -- which is the point of having a manifest. */
    params_.attach(spec_.params, apvts_);
    //  Which plugin's parameters the shared engine is reading, set before anything else it does.
    mod_.useManifest(spec_.mod);
}

PluginProcessor::~PluginProcessor() = default;

void PluginProcessor::begin(PluginState initial) {
    params_.intake();
    initial.params = params_.values;
    initial.preset = kDefaultPreset;  // a fresh instance is the default preset
    fresh_ = initial;
    handoff_.begin(buildSnapshot(initial, true));
    mod_.usePatch(handoff_.current()->patch);
    committing(*handoff_.current());
    document_.initialise(initial);

    //  Joining the session waits for the first timer ticks. Failing to open the directory is fine:
    //  a new instance then starts a session of its own.
    link_.start();
    linkTimer_.startTimerHz(LinkNode::kTickHz);
}

// ---- state ---------------------------------------------------------------------------

std::unique_ptr<PluginProcessor::EngineSnapshot> PluginProcessor::buildSnapshot(const PluginState& st,
                                                                                bool snap) const {
    auto snapshot = newSnapshot(st);
    snapshot->patch = ModulationPatch::compile(spec_.mod, st);
    snapshot->regions = st.regions;
    snapshot->renderQuality = st.renderQuality;
    snapshot->room = st.room;
    snapshot->snap = snap;
    return snapshot;
}

void PluginProcessor::commitDocument(PluginState next, bool snap) {
    //  Everything that allocates happens here, before the audio thread can see any of it.
    auto snapshot = buildSnapshot(next, snap);
    std::unique_ptr<EngineSnapshot> displaced;
    /*  Sequence, document and hand-over under one lock, so two commits racing from two threads
        cannot leave the audio thread on the older one. The audio thread never takes it. */
    document_.takeCommitted(std::move(next), [&](std::uint64_t sequence, PluginState&) {
        snapshot->sequence = sequence;
        committing(*snapshot);
        displaced = handoff_.post(std::move(snapshot));
    });
    displaced.reset();  // outside the lock: a delete is not something to do under one
    link_.markStaticDirty();
}

PluginState PluginProcessor::documentState() const {
    PluginState st = document_.committed();
    st.identity = identity();
    st.windowScale = windowScale();
    for (int i = 0; i < spec_.params.size(); ++i) st.params[static_cast<std::size_t>(i)] = params_.live(i);
    return st;
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const PluginState st = documentState();
    const std::string text = saveState(spec_.product, spec_.params, st);
    destData.replaceAll(text.data(), text.size());
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) return;
    const std::string text(static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes));
    PluginState st{spec_.params};
    //  A state another plugin wrote is refused by product, not half-read.
    if (!loadState(spec_.product, spec_.params, text, st).ok) return;

    /*  Identity comes from state only before this instance has joined: a project loading, or a
        track being duplicated. State loaded into a running instance -- a preset, the host's undo --
        must not move it to another session or hand it another instance's id. */
    identity_.fromState(st.identity);
    setWindowScale(st.windowScale);  // what the next window opens at; one open now keeps its size

    //  A loaded state is a new document: the history before it is not this document's.
    document_.adopt(st);

    //  The parameters first, as a whole step's commit does: the block that snaps to the new patch
    //  reads them. No callback lock: the audio thread adopts the patch at its next block.
    setHostParameters(st.params);
    commitDocument(std::move(st), true);
}

void PluginProcessor::setHostParameters(const std::array<float, kMaxParams>& values) {
    //  Through the host's own parameters, so the host sees them; only the ones that move.
    for (int i = 0; i < spec_.params.size(); ++i)
        if (auto* param = params_.object(i)) {
            const float normalised = param->convertTo0to1(values[static_cast<std::size_t>(i)]);
            if (std::abs(param->getValue() - normalised) > 1.0e-7f) param->setValueNotifyingHost(normalised);
        }
}

// ---- presets ---------------------------------------------------------------------------

std::filesystem::path PluginProcessor::presetRoot(Product p) {
    std::string plugin(name(p));
    if (!plugin.empty()) plugin[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plugin[0])));
    if (const char* dir = std::getenv("BAMBI_PRESET_DIR"); dir != nullptr && *dir != 0)
        return std::filesystem::path(dir) / plugin;
    auto data = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    data = data.getChildFile("Application Support");  // JUCE's answer on a Mac is ~/Library
#endif
    return std::filesystem::path(data.getFullPathName().toStdString()) / "bambi" / "Presets" / plugin;
}

void PluginProcessor::usePresetDirectory(const std::filesystem::path& dir) {
#if JUCE_WINDOWS
    _putenv_s("BAMBI_PRESET_DIR", dir.string().c_str());  // an empty value removes it
#else
    if (dir.empty())
        unsetenv("BAMBI_PRESET_DIR");
    else
        setenv("BAMBI_PRESET_DIR", dir.string().c_str(), 1);
#endif
}

PresetLibrary& PluginProcessor::presets() {
    JUCE_ASSERT_MESSAGE_THREAD
    if (presets_ == nullptr) {
        //  a check's directory is deleted from outright: its files are not for the user's Trash
        const char* checks = std::getenv("BAMBI_PRESET_DIR");
        PresetLibrary::Dispose dispose;
        if (checks == nullptr || *checks == 0)
            dispose = [](const std::filesystem::path& file) {
                return juce::File(juce::String(file.string())).moveToTrash();
            };
        presets_ = std::make_unique<PresetLibrary>(spec_.product, spec_.params, fresh_, spec_.factory,
                                                   presetRoot(spec_.product), std::move(dispose));
    }
    return *presets_;
}

PresetRef PluginProcessor::currentPreset() const {
    return document_.withEditing([](const PluginState& s, const UndoStack&) { return s.preset; });
}

bool PluginProcessor::presetModified() { return presets().modified(documentState()); }

bool PluginProcessor::loadPreset(const PresetRef& ref) {
    const auto preset = presets().load(ref);
    if (!preset.has_value()) return false;
    document_.editWhole("load preset",
                        [&](PluginState& s) { adoptPreset(spec_.product, spec_.params, *preset, ref, s); });
    link_.noteActivity();
    return true;
}

bool PluginProcessor::stepPreset(int delta) {
    return loadPreset(bambi::stepPreset(presets().all(), currentPreset(), delta));
}

bool PluginProcessor::savePreset(const PresetRef& ref) {
    if (!presets().save(ref, documentState())) return false;
    //  only the patch being edited takes the name: the steps before it were other presets'
    document_.amend([&](PluginState& s) { s.preset = ref; });
    return true;
}

bool PluginProcessor::renamePreset(const PresetRef& from, const PresetRef& to) {
    if (!presets().rename(from, to)) return false;
    document_.amend(
        [&](PluginState& s) {
            if (s.preset == from) s.preset = to;
        },
        true);
    return true;
}

bool PluginProcessor::removePreset(const PresetRef& ref) { return presets().remove(ref); }

void PluginProcessor::adoptPendingSnapshot() noexcept {
    const EngineSnapshot* next = handoff_.adopt();
    diag_.adoptionsDeferred.store(handoff_.deferrals(), std::memory_order_relaxed);
    if (next == nullptr) return;
    mod_.usePatch(next->patch);
    snapshotAdopted(*next);
    diag_.stateSequence.store(next->sequence, std::memory_order_relaxed);
}

void PluginProcessor::armNoteLearn(int envelope) noexcept {
    learnArmed_.store(envelope >= 0 && envelope < kNumEnvelopes ? envelope : -1, std::memory_order_relaxed);
}

void PluginProcessor::catchLearnedNote(const juce::MidiBuffer& midi) noexcept {
    int armed = learnArmed_.load(std::memory_order_relaxed);
    if (armed < 0) return;  // almost always: nothing to look for, and the buffer is not walked
    for (const auto metadata : midi) {
        const auto message = metadata.getMessage();
        if (!message.isNoteOn()) continue;
        if (learnArmed_.compare_exchange_strong(armed, -1, std::memory_order_relaxed))
            learned_.store(armed << 16 | message.getChannel() << 8 | message.getNoteNumber(),
                           std::memory_order_relaxed);
        return;  // only the first note-on counts
    }
}

bool PluginProcessor::takeLearnedNote(int& envelope, int& channel, int& note) noexcept {
    const int packed = learned_.exchange(-1, std::memory_order_relaxed);
    if (packed < 0) return false;
    envelope = packed >> 16;
    channel = (packed >> 8) & 0xff;
    note = packed & 0xff;
    return true;
}

void PluginProcessor::linkTick() {
    jassert(handoff_.current() != nullptr);  // a constructor that did not end with begin()
    handoff_.freeRetired();

    //  A learned note becomes an edit here, so it undoes like any other.
    int envelope = 0, channel = 0, note = 0;
    if (takeLearnedNote(envelope, channel, note)) {
        document_.edit("learn note", [envelope, channel, note](PluginState& s) {
            learnTriggerNote(s.envTriggers[static_cast<std::size_t>(envelope)], channel, note);
        });
    }
    onLinkTick();
    link_.tick();
    shareEnergy();
}

void PluginProcessor::shareEnergy() {
    const bool asked = link_.energyAskedFor();
    remoteWanted_.store(asked, std::memory_order_relaxed);
    const auto now = covGeneration_.load(std::memory_order_acquire);
    if (!asked || now == covBusTaken_) return;
    covBusTaken_ = now;
    //  The upper triangle of the first (kLinkEnergyOrder + 1)^2 channels, out of one built at any order.
    const int built = energyOrder_.load(std::memory_order_relaxed);
    const int order = std::min(built, kLinkEnergyOrder);
    const int from = (built + 1) * (built + 1);
    const int to = (order + 1) * (order + 1);
    auto& e = busEnergy_;
    e.order = static_cast<std::uint8_t>(order);
    e.withArrived = withArrived_ ? 1 : 0;
    e.count = static_cast<std::uint32_t>(covarianceSize(order));
    for (int i = 0; i < to; ++i)
        for (int j = i; j < to; ++j) {
            const auto src = static_cast<std::size_t>(covarianceIndex(from, i, j));
            const auto dst = static_cast<std::size_t>(covarianceIndex(to, i, j));
            e.added[dst] = src < addedCell_.size() ? addedCell_[src] : 0.0f;
            e.arrived[dst] = src < arrivedCell_.size() ? arrivedCell_[src] : 0.0f;
        }
    link_.publishEnergy(e);
}

// ---- audio ---------------------------------------------------------------------------

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    load_.begin();
    const SnapshotCensus::InAudioBlock inBlock;
    renderBlock(buffer, midi);
    load_.end(buffer.getNumSamples(), loadRate_);
}

void PluginProcessor::prepareEnergy(int order, double sampleRate, bool withArrived) {
    //  50 ms of field a window. Sized here and never again.
    const int window = std::max(1, static_cast<int>(sampleRate * 0.05));
    const int built = std::max(0, order);
    withArrived_ = withArrived;
    energyOrder_.store(built, std::memory_order_relaxed);
    addedWindow_.prepare(built, window);
    addedCell_.assign(static_cast<std::size_t>(covarianceSize(built)), 0.0f);
    arrivedWindow_.prepare(withArrived ? built : 0, withArrived ? window : 1);
    arrivedCell_.assign(withArrived ? addedCell_.size() : 0, 0.0f);
}

void PluginProcessor::resetEnergy() noexcept {
    addedWindow_.reset();
    arrivedWindow_.reset();
}

void PluginProcessor::publishEnergy() noexcept {
    if (!addedWindow_.ready() || (withArrived_ && !arrivedWindow_.ready())) return;
    addedWindow_.take(addedCell_);
    if (withArrived_) arrivedWindow_.take(arrivedCell_);
    covGeneration_.fetch_add(1, std::memory_order_release);
}

bool PluginProcessor::takeCovariances(std::vector<float>& added, std::vector<float>& arrived) {
    const auto now = covGeneration_.load(std::memory_order_acquire);
    if (now == covTaken_) return false;
    covTaken_ = now;
    added = addedCell_;
    arrived = arrivedCell_;
    return true;
}

void PluginProcessor::publishTurn(int slot, const RotationClock& turn) noexcept {
    if (slot < 0 || slot >= kMaxRegions) return;
    const auto at = static_cast<std::size_t>(slot * 3);
    diag_.regionTurn[at + 0].store(static_cast<float>(turn.yawRad()), std::memory_order_relaxed);
    diag_.regionTurn[at + 1].store(static_cast<float>(turn.pitchRad()), std::memory_order_relaxed);
    diag_.regionTurn[at + 2].store(static_cast<float>(turn.rollRad()), std::memory_order_relaxed);
}

void PluginProcessor::publishSources() noexcept {
    for (int i = 0; i < kNumSources; ++i)
        diag_.sourceValues[static_cast<std::size_t>(i)].store(static_cast<float>(mod_.sourceValue(i)),
                                                              std::memory_order_relaxed);
}

}  // namespace bambi::host
