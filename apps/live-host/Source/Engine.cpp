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

std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Engine::~Engine() {
    devices_.removeAudioCallback(this);
    devices_.closeAudioDevice();
    for (auto& v : voices_) v.encoder->releaseResources();
    if (decoder_ != nullptr) decoder_->releaseResources();
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
    decoder_ = std::move(decoded.plugin);
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

bool Engine::setTracks(const std::vector<live::TrackPair>& pairs, juce::String& status) {
    std::vector<Voice> next;
    bool changed = false;
    for (const live::TrackPair& tp : pairs) {
        if (tp.pair < 0 || tp.pair >= s_.pairs) continue;
        const auto had =
            std::find_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.track == tp.track; });
        if (had != voices_.end()) {
            next.push_back({tp.track, tp.pair, had->encoder});
            changed = changed || had->pair != tp.pair;
            continue;
        }
        auto loaded = loadInstalled("bambi Encoder", Role::Encoder, s_.order, s_.rate, block_);
        if (loaded.plugin == nullptr) {
            status = loaded.status;
            continue;
        }
        std::shared_ptr<juce::AudioPluginInstance> encoder = std::move(loaded.plugin);
        decode(encoder.get(), waiting_.find(keyOf(tp.track)));
        waiting_.erase(keyOf(tp.track));
        next.push_back({tp.track, tp.pair, std::move(encoder)});
        changed = true;
    }
    changed = changed || next.size() != voices_.size();
    if (!changed) return false;

    live::Plan plan;
    std::vector<juce::AudioProcessor*> slots;
    Runner::Keep keep;
    for (const Voice& v : next) {
        live::FieldNode n;
        n.track = v.track;
        n.pair = v.pair;
        n.encoder = static_cast<int>(slots.size());
        plan.nodes.push_back(n);
        plan.slots.push_back({v.encoder->getLatencySamples()});
        slots.push_back(v.encoder.get());
        keep.push_back(v.encoder);
    }
    plan.decoder = static_cast<int>(slots.size());
    plan.slots.push_back({decoder_->getLatencySamples()});
    slots.push_back(decoder_.get());
    keep.push_back(decoder_);

    std::string error;
    const auto compiled = live::compile(plan, &error);
    juce::String why;
    if (!compiled || !runner_.swap(*compiled, slots, 0, why, std::move(keep))) {
        status = "the plan: " + (compiled ? why : juce::String(error));
        return false;
    }
    //  the Encoders only the old program plays stay alive in it until the audio thread lets go of it
    voices_ = std::move(next);
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    status = juce::String(static_cast<int>(voices_.size())) + " Encoders and the Decoder";
    return true;
}

Json Engine::states() const {
    Json encoders = waiting_;  // a restored state whose track is not on a pair now is kept for when it is
    for (const Voice& v : voices_) encoders.set(keyOf(v.track), Json(encoded(v.encoder.get()).toStdString()));
    Json all = Json::object();
    all.set("encoders", std::move(encoders));
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
