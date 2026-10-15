// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Plugins.h"
#include "bambi/live/graph.hpp"

namespace bambi::livehost {

namespace {

constexpr std::int64_t kFadeIn = 4800;  ///< samples from silence to the mix, at every start

juce::String encoded(juce::AudioProcessor* p) {
    juce::MemoryBlock block;
    if (p != nullptr) p->getStateInformation(block);
    return block.toBase64Encoding();
}

//  A state as the set kept it, into `p`; one missing or unreadable leaves it as it is.
void decode(juce::AudioProcessor* p, const Json* state) {
    juce::MemoryBlock block;
    if (p == nullptr || state == nullptr || !state->isString() ||
        !block.fromBase64Encoding(juce::String(std::string(state->stringOr("")))) || block.getSize() == 0)
        return;
    p->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
}

std::string keyOf(live::TrackId track) { return std::to_string(track); }

//  A loaded plugin, shared by the programs that play it, released and freed by whichever lets go of it last -- on
//  the message thread, where programs are freed.
std::shared_ptr<juce::AudioPluginInstance> shared(std::unique_ptr<juce::AudioPluginInstance> plugin) {
    return {plugin.release(), [](juce::AudioPluginInstance* p) {
                p->releaseResources();
                delete p;
            }};
}

std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Engine::~Engine() {
    devices_.removeAudioCallback(this);
    devices_.closeAudioDevice();
}

bool Engine::start(const Settings& settings, juce::String& status) {
    s_ = settings;
    const int channels = 2 * (s_.pairs + 1);
    returnChannel_ = 2 * s_.pairs;

    //  The device first: the plugins are prepared at what it runs at. Opened by name, never the system's default,
    //  which may be one -- a Bluetooth headset -- that takes its time, or hangs, to open.
    juce::String opened;
    for (const char* name : {"bambi", "BlackHole 16ch"}) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = name;
        setup.sampleRate = s_.rate;
        setup.bufferSize = s_.bufferSize;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange(0, channels, true);
        setup.outputChannels.setRange(0, channels, true);
        if (devices_.initialise(channels, channels, nullptr, false, {}, &setup).isEmpty() &&
            devices_.getCurrentAudioDevice() != nullptr && devices_.getCurrentAudioDevice()->getName() == name) {
            opened = name;
            break;
        }
    }
    auto* device = devices_.getCurrentAudioDevice();
    if (opened.isEmpty() || device == nullptr) {
        status = "no loop device: neither bambi nor BlackHole 16ch could be opened";
        devices_.closeAudioDevice();
        return false;
    }
    s_.rate = device->getCurrentSampleRate();
    block_ = device->getCurrentBufferSizeSamples();

    auto decoded = loadInstalled("bambi Decoder", Role::Decoder, s_.order, s_.rate, block_, 2);
    if (decoded.plugin == nullptr) {
        status = decoded.status;
        return false;
    }
    decoder_ = shared(std::move(decoded.plugin));
    live::Plan plan;
    plan.slots = {{decoder_->getLatencySamples()}};
    plan.decoder = 0;
    std::string error;
    const auto compiled = live::compile(plan, &error);
    juce::String why;
    if (!compiled ||
        !runner_.prepare(*compiled, {decoder_.get()}, 0, {s_.order, block_, channels, 2, s_.rate}, why, {decoder_})) {
        status = "the first program: " + (compiled ? why : juce::String(error));
        return false;
    }
    projector_ = live::TransportProjector({s_.rate});
    arrivals_.emplace(s_.rate, s_.transportOffsetSamples);
    runner_.setClock(&projector_);
    loopChannels_ = channels;

    deviceLatency_ = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() + block_;
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    devices_.addAudioCallback(this);
    status = opened + ", " + juce::String(s_.rate / 1000.0) + " kHz, " + juce::String(block_) + " samples, order " +
             juce::String(s_.order) + ": the Decoder, " + juce::String(latencyMs_, 1) + " ms here";
    return true;
}

bool Engine::setField(const live::Field& field, const std::vector<live::TrackPair>& pairs, juce::String& status) {
    live::FieldPlan fp = live::planOf(field, pairs);
    //  the restored states the new plugins took: given up only once the swap is made, so a change that fails part
    //  of the way leaves them for the next
    std::vector<std::string> restoredEncoders, restoredEffects;
    //  each source's Encoder: the one it had, or one loaded now
    std::vector<Voice> next;
    std::vector<int> drop;  // nodes whose Encoder could not be loaded
    for (std::size_t slot = 0; slot < fp.encoders.size(); ++slot) {
        const live::TrackId track = fp.encoders[slot];
        const auto node = std::find_if(fp.plan.nodes.begin(), fp.plan.nodes.end(),
                                       [&](const live::FieldNode& n) { return n.encoder == static_cast<int>(slot); });
        const int pair = node->pair;
        const auto had = std::find_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.track == track; });
        if (had != voices_.end()) {
            next.push_back({track, pair, had->encoder});
            continue;
        }
        auto loaded = loadInstalled("bambi Encoder", Role::Encoder, s_.order, s_.rate, block_);
        if (loaded.plugin == nullptr) {
            status = loaded.status;
            return false;
        }
        std::shared_ptr<juce::AudioPluginInstance> encoder = shared(std::move(loaded.plugin));
        runner_.ready(encoder.get());
        decode(encoder.get(), waiting_.find(keyOf(track)));
        restoredEncoders.push_back(keyOf(track));
        next.push_back({track, pair, std::move(encoder)});
    }
    //  each insert's Echo or Reverb: the one it had, or one loaded now
    std::vector<Fx> nextEffects;
    for (const live::Insert& insert : fp.effects) {
        const auto had =
            std::find_if(effects_.begin(), effects_.end(), [&](const Fx& f) { return f.insert.id == insert.id; });
        if (had != effects_.end() && had->insert.effect == insert.effect) {
            nextEffects.push_back(*had);
            continue;
        }
        auto loaded = loadInstalled(insert.effect == live::Effect::Reverb ? "bambi Reverb" : "bambi Echo", Role::Effect,
                                    s_.order, s_.rate, block_);
        if (loaded.plugin == nullptr) {
            status = loaded.status;
            return false;
        }
        std::shared_ptr<juce::AudioPluginInstance> plugin = shared(std::move(loaded.plugin));
        runner_.ready(plugin.get());
        decode(plugin.get(), waitingEffects_.find(keyOf(insert.id)));
        restoredEffects.push_back(keyOf(insert.id));
        nextEffects.push_back({insert, std::move(plugin)});
    }
    const auto sidechains = [](const live::Plan& p) {
        std::vector<int> out;
        for (const live::Slot& slot : p.slots) out.push_back(slot.sidechain);
        return out;
    };
    if (fp.plan.nodes == plan_.nodes && fp.plan.main == plan_.main && fp.effects == playing_ &&
        sidechains(fp.plan) == sidechains(plan_) && fp.plan.returns.size() == plan_.returns.size() &&
        std::equal(fp.plan.returns.begin(), fp.plan.returns.end(), plan_.returns.begin(),
                   [](const live::Return& a, const live::Return& b) { return a.chain == b.chain; }))
        return false;  // the same shape, the same plugins

    std::vector<juce::AudioProcessor*> slots;
    Runner::Keep keep;
    const auto add = [&](const std::shared_ptr<juce::AudioPluginInstance>& plugin) {
        fp.plan.slots[slots.size()].latency = plugin->getLatencySamples();
        slots.push_back(plugin.get());
        keep.push_back(plugin);
    };
    for (const Voice& v : next) add(v.encoder);
    for (const Fx& f : nextEffects) add(f.plugin);
    add(decoder_);

    std::string error;
    const auto compiled = live::compile(fp.plan, &error);
    juce::String why;
    if (!compiled || !runner_.swap(*compiled, slots, compiled->levels, why, std::move(keep))) {
        status = "the plan: " + (compiled ? why : juce::String(error));
        return false;
    }
    //  the Encoders only the old program plays stay alive in it until the audio thread lets go of it
    for (const auto& key : restoredEncoders) waiting_.erase(key);
    for (const auto& key : restoredEffects) waitingEffects_.erase(key);
    voices_ = std::move(next);
    effects_ = std::move(nextEffects);
    playing_ = fp.effects;
    plan_ = fp.plan;
    fieldPlan_ = std::move(fp);
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    int buses = 0;
    for (const live::FieldNode& n : plan_.nodes) buses += n.pair < 0 ? 1 : 0;
    status =
        juce::String(static_cast<int>(voices_.size())) + " Encoders, " + juce::String(buses) + " buses and the Decoder";
    return true;
}

void Engine::setSends(const live::Field& field, const live::Tree& tree) {
    const std::vector<float> levels = live::levels(fieldPlan_, field, tree);
    for (std::size_t i = 0; i < levels.size(); ++i) runner_.setLevel(static_cast<int>(i), levels[i]);
}

Json Engine::states() const {
    Json encoders = waiting_;  // a restored state whose track is not on a pair now is kept for when it is
    for (const Voice& v : voices_) encoders.set(keyOf(v.track), Json(encoded(v.encoder.get()).toStdString()));
    Json effects = waitingEffects_;
    for (const Fx& f : effects_) effects.set(keyOf(f.insert.id), Json(encoded(f.plugin.get()).toStdString()));
    Json all = Json::object();
    all.set("encoders", std::move(encoders));
    all.set("effects", std::move(effects));
    all.set("decoder", Json(encoded(decoder_.get()).toStdString()));
    return all;
}

void Engine::restore(const Json& states) {
    waiting_ = Json::object();
    if (const Json* encoders = states.find("encoders"); encoders != nullptr && encoders->isObject())
        for (const auto& [track, state] : encoders->members()) {
            const auto v =
                std::find_if(voices_.begin(), voices_.end(), [&](const Voice& x) { return keyOf(x.track) == track; });
            if (v != voices_.end())
                decode(v->encoder.get(), &state);
            else
                waiting_.set(track, state);
        }
    waitingEffects_ = Json::object();
    if (const Json* effects = states.find("effects"); effects != nullptr && effects->isObject())
        for (const auto& [id, state] : effects->members()) {
            const auto f =
                std::find_if(effects_.begin(), effects_.end(), [&](const Fx& x) { return keyOf(x.insert.id) == id; });
            if (f != effects_.end())
                decode(f->plugin.get(), &state);
            else
                waitingEffects_.set(id, state);
        }
    decode(decoder_.get(), states.find("decoder"));
}

void Engine::transport(const live::TransportMessage& message, std::int64_t stampNs) noexcept {
    (void)queue_.push({message, stampNs});
}

juce::AudioProcessor* Engine::encoder(int pair) const {
    for (const Voice& v : voices_)
        if (v.pair == pair) return v.encoder.get();
    return nullptr;
}

juce::AudioProcessor* Engine::firstEncoder() const {
    const Voice* first = nullptr;
    for (const Voice& v : voices_)
        if (first == nullptr || v.pair < first->pair) first = &v;
    return first == nullptr ? nullptr : first->encoder.get();
}

juce::AudioProcessor* Engine::encoderOf(live::TrackId track) const {
    for (const Voice& v : voices_)
        if (v.track == track) return v.encoder.get();
    return nullptr;
}

juce::AudioProcessor* Engine::effectOf(std::uint64_t insert) const {
    for (const Fx& f : effects_)
        if (f.insert.id == insert) return f.plugin.get();
    return nullptr;
}

juce::AudioProcessor* Engine::firstEffect(live::Effect effect) const {
    for (const Fx& f : effects_)
        if (f.insert.effect == effect) return f.plugin.get();
    return nullptr;
}

bool Engine::holds(const juce::AudioProcessor* p) const {
    if (p != nullptr && p == decoder_.get()) return true;
    if (std::any_of(effects_.begin(), effects_.end(), [&](const Fx& f) { return f.plugin.get() == p; })) return true;
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.encoder.get() == p; });
}

void Engine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    playable_.store(device != nullptr && std::abs(device->getCurrentSampleRate() - s_.rate) < 0.5,
                    std::memory_order_relaxed);
    faded_ = 0;
}

void Engine::audioDeviceStopped() { playable_.store(false, std::memory_order_relaxed); }

void Engine::audioDeviceIOCallbackWithContext(const float* const* inputs, int inputCount, float* const* outputs,
                                              int outputCount, int frames, const juce::AudioIODeviceCallbackContext&) {
    for (int ch = 0; ch < outputCount; ++ch)
        if (outputs[ch] != nullptr) juce::FloatVectorOperations::clear(outputs[ch], frames);
    if (!playable_.load(std::memory_order_relaxed) || outputCount < returnChannel_ + 2) return;

    const std::int64_t now = nowNs();
    for (Timed t; queue_.pop(t);)
        projector_.receive(t.message, arrivals_->at(runner_.sample(), 1e-9 * static_cast<double>(now - t.stampNs)));

    //  The device's own channels, referred to rather than copied. A device giving fewer plays nothing.
    if (inputCount < loopChannels_) return;
    const juce::AudioBuffer<float> loop(const_cast<float* const*>(inputs), loopChannels_, frames);
    float* pair[2] = {outputs[returnChannel_], outputs[returnChannel_ + 1]};
    juce::AudioBuffer<float> out(pair, 2, frames);
    runner_.process(loop, out, frames);

    if (faded_ < kFadeIn) {
        const int n = static_cast<int>(std::min<std::int64_t>(frames, kFadeIn - faded_));
        const float from = static_cast<float>(faded_) / kFadeIn, to = static_cast<float>(faded_ + n) / kFadeIn;
        out.applyGainRamp(0, n, from, to);
        faded_ += n;
    }
}

}  // namespace bambi::livehost
