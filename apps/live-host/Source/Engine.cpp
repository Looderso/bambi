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

std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Engine::~Engine() {
    devices_.removeAudioCallback(this);
    devices_.closeAudioDevice();
    for (auto& p : plugins_)
        if (p != nullptr) p->releaseResources();
}

bool Engine::start(const Settings& settings, juce::String& status) {
    s_ = settings;
    const int channels = 2 * (s_.pairs + 1);
    returnChannel_ = 2 * s_.pairs;

    //  The device first: the plugins are prepared at what it runs at.
    if (const juce::String error = devices_.initialiseWithDefaultDevices(channels, channels); error.isNotEmpty()) {
        status = "no audio: " + error;
        return false;
    }
    juce::String opened;
    for (const char* name : {"bambi", "BlackHole 16ch"}) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = name;
        setup.sampleRate = s_.rate;
        setup.bufferSize = s_.bufferSize;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange(0, channels, true);
        setup.outputChannels.setRange(0, channels, true);
        if (devices_.setAudioDeviceSetup(setup, true).isEmpty() && devices_.getCurrentAudioDevice() != nullptr &&
            devices_.getCurrentAudioDevice()->getName() == name) {
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
    const int block = device->getCurrentBufferSizeSamples();

    live::Plan plan;
    for (int pair = 0; pair < s_.pairs; ++pair) {
        auto loaded = loadInstalled("bambi Encoder", Role::Encoder, s_.order, s_.rate, block);
        if (loaded.plugin == nullptr) {
            status = loaded.status;
            return false;
        }
        plugins_.push_back(std::move(loaded.plugin));
        live::FieldNode n;
        n.track = static_cast<live::TrackId>(pair + 1);
        n.pair = pair;
        n.encoder = pair;
        plan.nodes.push_back(n);
    }
    auto decoded = loadInstalled("bambi Decoder", Role::Decoder, s_.order, s_.rate, block, 2);
    if (decoded.plugin == nullptr) {
        status = decoded.status;
        return false;
    }
    plugins_.push_back(std::move(decoded.plugin));
    plan.decoder = s_.pairs;
    for (auto& p : plugins_) plan.slots.push_back({p->getLatencySamples()});

    std::string error;
    const auto compiled = live::compile(plan, &error);
    if (!compiled) {
        status = "the plan: " + juce::String(error);
        return false;
    }
    std::vector<juce::AudioProcessor*> slots;
    for (auto& p : plugins_) slots.push_back(p.get());
    juce::String why;
    if (!runner_.prepare(*compiled, slots, 0, {s_.order, block, channels, 2, s_.rate}, why)) {
        status = "the runner: " + why;
        return false;
    }
    projector_ = live::TransportProjector({s_.rate});
    arrivals_.emplace(s_.rate, s_.transportOffsetSamples);
    runner_.setClock(&projector_);
    loopChannels_ = channels;

    const int lateness =
        device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() + block + compiled->latency;
    latencyMs_ = 1000.0 * lateness / s_.rate;
    devices_.addAudioCallback(this);
    status = opened + ", " + juce::String(s_.rate / 1000.0) + " kHz, " + juce::String(block) +
             " samples: " + juce::String(s_.pairs) + " Encoders and the Decoder at order " + juce::String(s_.order) +
             ", " + juce::String(latencyMs_, 1) + " ms here";
    return true;
}

void Engine::transport(const live::TransportMessage& message, std::int64_t stampNs) noexcept {
    (void)queue_.push({message, stampNs});
}

juce::AudioProcessor* Engine::encoder(int pair) const {
    return pair >= 0 && pair < s_.pairs && pair < static_cast<int>(plugins_.size())
               ? plugins_[static_cast<std::size_t>(pair)].get()
               : nullptr;
}

juce::AudioProcessor* Engine::decoder() const {
    return static_cast<int>(plugins_.size()) == s_.pairs + 1 ? plugins_.back().get() : nullptr;
}

void Engine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    playable_.store(device != nullptr && std::abs(device->getCurrentSampleRate() - s_.rate) < 0.5, std::memory_order_relaxed);
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
