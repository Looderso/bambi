// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Plugins.h"
#include "bambi/decode/remote.hpp"
#include "bambi/live/graph.hpp"
#include "loop.hpp"

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

std::vector<live::PlayingDecoder> Engine::playing() const {
    std::vector<live::PlayingDecoder> out;
    for (const Dec& d : decoders_) {
        DecoderReport report;
        report.headphonesOn = true;  // a Decoder that offers no remote interface: its fresh outputs
        if (auto* remote = remoteOf(*d.plugin, kDecoderRemoteKind, kDecoderRemoteVersion))
            report = reportFrom(Json::parse(remoteReport(*remote)));
        out.push_back({d.id, std::move(report), d.plugin->getLatencySamples()});
    }
    return out;
}

std::shared_ptr<juce::AudioPluginInstance> Engine::loadDecoder(juce::String& status) {
    //  as wide as a VST3 bus is: the Decoder places its outputs on it by its own rules, and never runs out of room
    auto loaded = loadInstalled("bambi Decoder", Role::Decoder, s_.order, s_.rate, block_, kMaxVst3BusChannels);
    if (loaded.plugin == nullptr) {
        status = loaded.status;
        return nullptr;
    }
    std::shared_ptr<juce::AudioPluginInstance> plugin = shared(std::move(loaded.plugin));
    runner_.ready(plugin.get());
    return plugin;
}

std::vector<Runner::Route> Engine::routesFor(int fieldLatency) const {
    std::vector<Runner::Route> out;
    for (const live::OutputRoute& r : live::planOutputs(s_.audio, row_, playing(), fieldLatency).routes) {
        const int to = r.to == live::OutputRoute::To::Row ? loopChannels_ + r.dest - 1 : returnChannel_ + r.dest - 1;
        if (to >= 0 && to < outputChannels_)
            out.push_back({r.from == live::OutputRoute::From::Main, r.decoder, r.channel - 1, to, r.delay});
    }
    return out;
}

Engine::~Engine() {
    devices_.removeAudioCallback(this);
    devices_.closeAudioDevice();
}

bool Engine::open(juce::String& status) {
    const int channels = 2 * (s_.pairs + 1);
    //  Opened by name, never the system's default, which may be one -- a Bluetooth headset -- that takes its time,
    //  or hangs, to open: the aggregate of the loop device and the chosen devices; while none is chosen, or it cannot
    //  be made, the loop device Live reaches, through the aggregate it plays on; failing that, bambi's own, else
    //  BlackHole
    const auto joined = live::aggregateOf(s_.audio.devices, devices::system());
    std::vector<std::string> uids;
    for (const live::AudioDevice& d : joined) uids.push_back(d.uid);
    std::string aggregateError;
    const int loopWidth = devices::loopChannels();
    const bool aggregated = !joined.empty() && loopWidth >= channels && aggregate_.create(uids, &aggregateError);
    if (aggregated)
        for (juce::AudioIODeviceType* type : devices_.getAvailableDeviceTypes()) type->scanForDevices();
    row_ = aggregated ? live::rowOf(joined) : live::Row{};
    inputRow_ = aggregated ? live::rowOf(joined, true) : live::Row{{}, true};
    loopChannels_ = aggregated ? loopWidth : channels;
    inputChannels_ = loopChannels_ + inputRow_.width();
    outputChannels_ = loopChannels_ + row_.width();
    juce::String opened;
    std::vector<juce::String> names;
    if (aggregated) names.push_back(devices::Aggregate::kName);
    if (const auto held = loop::inAggregate(); !aggregated && !held.empty()) names.push_back(juce::String(held));
    for (const char* name : {"bambi", "BlackHole 16ch"})
        if (!aggregated && std::find(names.begin(), names.end(), juce::String(name)) == names.end())
            names.push_back(name);
    for (const juce::String& name : names) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = name;
        setup.sampleRate = s_.rate;
        setup.bufferSize = s_.audio.buffer;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange(0, inputChannels_, true);
        setup.outputChannels.setRange(0, outputChannels_, true);
        if (devices_.initialise(inputChannels_, outputChannels_, nullptr, false, {}, &setup).isEmpty() &&
            devices_.getCurrentAudioDevice() != nullptr && devices_.getCurrentAudioDevice()->getName() == name) {
            opened = name;
            break;
        }
    }
    auto* device = devices_.getCurrentAudioDevice();
    if (opened.isEmpty() || device == nullptr) {
        status = aggregated ? juce::String("the host's aggregate of its devices could not be opened")
                 : !aggregateError.empty()
                     ? "no aggregate: " + juce::String(aggregateError)
                     : juce::String("no loop device: neither bambi nor BlackHole 16ch could be opened");
        devices_.closeAudioDevice();
        aggregate_.destroy();
        return false;
    }
    s_.rate = device->getCurrentSampleRate();
    block_ = device->getCurrentBufferSizeSamples();
    opened_ = opened;
    //  the callback's buffers, while no audio runs: a device may hand over more than its buffer at once, played in
    //  pieces of these
    const int piece = std::max(block_, 1024);
    in_.setSize(inputChannels_, piece);
    out_.setSize(outputChannels_, piece);
    deviceLatency_ = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() + block_;
    return true;
}

Runner::Setup Engine::runnerSetup() const {
    return {s_.order, block_, inputChannels_, outputChannels_, s_.rate, returnChannel_, static_cast<int>(s_.rate)};
}

bool Engine::start(const Settings& settings, juce::String& status) {
    s_ = settings;
    returnChannel_ = 2 * s_.pairs;
    //  the device first: the plugins are prepared at what it runs at
    if (!open(status)) return false;
    auto first = loadDecoder(status);
    if (first == nullptr) return false;
    decoders_ = {{live::kFirstDecoder, first}};
    prepared_ = block_;
    live::Plan plan;
    plan.slots = {{first->getLatencySamples()}};
    plan.decoders = {0};
    std::string error;
    const auto compiled = live::compile(plan, &error);
    juce::String why;
    if (compiled) routes_ = routesFor(compiled->latency);
    if (!compiled || !runner_.prepare(*compiled, {first.get()}, 0, runnerSetup(), why, {first}, routes_)) {
        status = "the first program: " + (compiled ? why : juce::String(error));
        return false;
    }
    program_ = {*compiled, {first.get()}, 0, {first}};
    projector_ = live::TransportProjector({s_.rate});
    arrivals_.emplace(s_.rate, s_.transportOffsetSamples);
    runner_.setClock(&projector_);
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    devices_.addAudioCallback(this);
    status = said();
    return true;
}

juce::String Engine::said() const {
    return opened_ + ", " + juce::String(s_.rate / 1000.0) + " kHz, " + juce::String(block_) + " samples, order " +
           juce::String(s_.order) + ": " + juce::String(latencyMs_, 1) + " ms here";
}

bool Engine::setAudio(const live::AudioSettings& audio, juce::String& status) {
    const bool reopen = audio.devices != s_.audio.devices || audio.buffer != s_.audio.buffer;
    s_.audio = audio;
    juce::String why;
    if (!reopen) {
        reroute();
        status = said();
        return true;
    }
    //  the device again, and every plugin laid out or prepared for it, while no audio runs
    devices_.removeAudioCallback(this);
    devices_.closeAudioDevice();
    aggregate_.destroy();
    if (!open(status)) return false;
    if (block_ > prepared_) {
        for (juce::AudioProcessor* plugin : program_.slots) plugin->prepareToPlay(s_.rate, block_);
        prepared_ = block_;
    }
    routes_ = routesFor(program_.compiled.latency);
    if (!runner_.prepare(program_.compiled, program_.slots, program_.levels, runnerSetup(), why, program_.keep,
                         routes_)) {
        status = "the program: " + why;
        return false;
    }
    latencyMs_ = 1000.0 * (deviceLatency_ + program_.compiled.latency) / s_.rate;
    devices_.addAudioCallback(this);
    status = said();
    return true;
}

bool Engine::reroute() {
    if (program_.slots.empty()) return false;
    auto routes = routesFor(program_.compiled.latency);
    if (routes == routes_) return false;  // a change the routes do not see is the Decoder's alone
    juce::String why;
    if (!runner_.swap(program_.compiled, program_.slots, program_.levels, why, program_.keep, routes)) return false;
    routes_ = std::move(routes);
    return true;
}

bool Engine::setField(const live::Field& field, const std::vector<live::TrackPair>& pairs,
                      const std::vector<live::HostInput>& inputs, juce::String& status) {
    auto arrivals = live::arrivalsOf(pairs);
    for (const live::Arrival& a : live::inputArrivals(s_.audio, inputs, inputRow_, loopChannels_)) arrivals.push_back(a);
    live::FieldPlan fp = live::planOf(field, arrivals);
    //  the restored states the new plugins took: given up only once the swap is made, so a change that fails part
    //  of the way leaves them for the next
    std::vector<std::string> restoredEncoders, restoredEffects;
    //  each source's Encoder: the one it had, or one loaded now
    std::vector<Voice> next;
    std::vector<int> drop;  // nodes whose Encoder could not be loaded
    for (std::size_t slot = 0; slot < fp.encoders.size(); ++slot) {
        const live::TrackId track = fp.encoders[slot];
        const auto on = std::find_if(pairs.begin(), pairs.end(), [&](const live::TrackPair& p) { return p.track == track; });
        const int pair = on == pairs.end() ? -1 : on->pair;  // an input of the host has none
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
    //  each Decoder: the one it had, or one loaded now
    std::vector<Dec> nextDecoders;
    std::vector<std::string> restoredDecoders;
    for (const std::uint64_t id : fp.decoders) {
        const auto had = std::find_if(decoders_.begin(), decoders_.end(), [&](const Dec& d) { return d.id == id; });
        if (had != decoders_.end()) {
            nextDecoders.push_back(*had);
            continue;
        }
        auto plugin = loadDecoder(status);
        if (plugin == nullptr) return false;
        decode(plugin.get(), waitingDecoders_.find(keyOf(id)));
        restoredDecoders.push_back(keyOf(id));
        nextDecoders.push_back({id, std::move(plugin)});
    }
    const auto ids = [](const std::vector<Dec>& list) {
        std::vector<std::uint64_t> out;
        for (const Dec& d : list) out.push_back(d.id);
        return out;
    };
    const auto sidechains = [](const live::Plan& p) {
        std::vector<int> out;
        for (const live::Slot& slot : p.slots) out.push_back(slot.sidechain);
        return out;
    };
    if (fp.plan.nodes == plan_.nodes && fp.plan.main == plan_.main && fp.effects == playing_ &&
        ids(nextDecoders) == ids(decoders_) &&
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
    for (const Dec& d : nextDecoders) add(d.plugin);

    std::string error;
    const auto compiled = live::compile(fp.plan, &error);
    juce::String why;
    auto routes = compiled ? routesFor(compiled->latency) : std::vector<Runner::Route>{};
    if (!compiled || !runner_.swap(*compiled, slots, compiled->levels, why, keep, routes)) {
        status = "the plan: " + (compiled ? why : juce::String(error));
        return false;
    }
    program_ = {*compiled, slots, compiled->levels, std::move(keep)};
    routes_ = std::move(routes);
    //  the Encoders only the old program plays stay alive in it until the audio thread lets go of it
    for (const auto& key : restoredEncoders) waiting_.erase(key);
    for (const auto& key : restoredEffects) waitingEffects_.erase(key);
    for (const auto& key : restoredDecoders) waitingDecoders_.erase(key);
    for (const Dec& d : decoders_)
        if (std::none_of(nextDecoders.begin(), nextDecoders.end(), [&](const Dec& x) { return x.id == d.id; }))
            waitingDecoders_.set(keyOf(d.id), Json(encoded(d.plugin.get()).toStdString()));
    //  a track that leaves the field, or an insert whose node does, keeps its state for when it comes back
    for (const Voice& v : voices_)
        if (std::none_of(next.begin(), next.end(), [&](const Voice& x) { return x.track == v.track; }))
            waiting_.set(keyOf(v.track), Json(encoded(v.encoder.get()).toStdString()));
    for (const Fx& f : effects_)
        if (std::none_of(nextEffects.begin(), nextEffects.end(),
                         [&](const Fx& x) { return x.insert.id == f.insert.id; }))
            waitingEffects_.set(keyOf(f.insert.id), Json(encoded(f.plugin.get()).toStdString()));
    voices_ = std::move(next);
    effects_ = std::move(nextEffects);
    decoders_ = std::move(nextDecoders);
    playing_ = fp.effects;
    plan_ = fp.plan;
    fieldPlan_ = std::move(fp);
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    int buses = 0;
    for (const live::FieldNode& n : plan_.nodes) buses += n.input < 0 ? 1 : 0;
    status = juce::String(static_cast<int>(voices_.size())) + " Encoders, " + juce::String(buses) + " buses, " +
             juce::String(static_cast<int>(decoders_.size())) + " Decoders";
    return true;
}

void Engine::setSends(const live::Field& field, const live::Tree& tree) {
    const std::vector<float> levels = live::levels(fieldPlan_, field, tree);
    for (std::size_t i = 0; i < levels.size(); ++i) runner_.setLevel(static_cast<int>(i), levels[i]);
}

void Engine::forget(const live::Tree& tree, const live::Shape& shape) {
    Json encoders = Json::object();
    for (const auto& [track, state] : waiting_.members())
        if (std::any_of(tree.nodes.begin(), tree.nodes.end(),
                        [&](const live::Node& n) { return keyOf(n.id) == track; }))
            encoders.set(track, state);
    waiting_ = std::move(encoders);
    Json effects = Json::object();
    for (const auto& [id, state] : waitingEffects_.members())
        for (const live::Chain& c : shape.chains)
            if (std::any_of(c.inserts.begin(), c.inserts.end(),
                            [&](const live::Insert& i) { return keyOf(i.id) == id; }))
                effects.set(id, state);
    waitingEffects_ = std::move(effects);
    Json decoders = Json::object();
    const auto kept = shape.decoders.empty() ? std::vector<std::uint64_t>{live::kFirstDecoder} : shape.decoders;
    for (const auto& [id, state] : waitingDecoders_.members())
        if (std::any_of(kept.begin(), kept.end(), [&](std::uint64_t d) { return keyOf(d) == id; }))
            decoders.set(id, state);
    waitingDecoders_ = std::move(decoders);
}

Json Engine::states() const {
    Json encoders = waiting_;  // a restored state whose track is not on a pair now is kept for when it is
    for (const Voice& v : voices_) encoders.set(keyOf(v.track), Json(encoded(v.encoder.get()).toStdString()));
    Json effects = waitingEffects_;
    for (const Fx& f : effects_) effects.set(keyOf(f.insert.id), Json(encoded(f.plugin.get()).toStdString()));
    Json all = Json::object();
    all.set("encoders", std::move(encoders));
    all.set("effects", std::move(effects));
    Json decoders = waitingDecoders_;
    for (const Dec& d : decoders_) decoders.set(keyOf(d.id), Json(encoded(d.plugin.get()).toStdString()));
    all.set("decoders", std::move(decoders));
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
    waitingDecoders_ = Json::object();
    //  a set from before there could be several: its one Decoder is the first
    if (const Json* one = states.find("decoder"))
        if (states.find("decoders") == nullptr) waitingDecoders_.set(keyOf(live::kFirstDecoder), *one);
    if (const Json* decoders = states.find("decoders"); decoders != nullptr && decoders->isObject())
        for (const auto& [id, state] : decoders->members()) waitingDecoders_.set(id, state);
    for (const Dec& d : decoders_)
        if (const Json* state = waitingDecoders_.find(keyOf(d.id))) {
            decode(d.plugin.get(), state);
            waitingDecoders_.erase(keyOf(d.id));
        }
}

void Engine::transport(const live::TransportMessage& message, std::int64_t stampNs) noexcept {
    (void)queue_.push({message, stampNs});
}

juce::AudioProcessor* Engine::decoderOf(std::uint64_t id) const {
    for (const Dec& d : decoders_)
        if (d.id == id) return d.plugin.get();
    return nullptr;
}

juce::AudioProcessor* Engine::encoder(int pair) const {
    for (const Voice& v : voices_)
        if (v.pair == pair) return v.encoder.get();
    return nullptr;
}

juce::AudioProcessor* Engine::firstEncoder() const {
    const Voice* first = nullptr;
    for (const Voice& v : voices_)
        if (v.pair >= 0 && (first == nullptr || v.pair < first->pair)) first = &v;
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
    for (const Dec& d : decoders_)
        if (p != nullptr && p == d.plugin.get()) return true;
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
    if (!playable_.load(std::memory_order_relaxed) || outputCount < outputChannels_) return;

    const std::int64_t now = nowNs();
    for (Timed t; queue_.pop(t);)
        projector_.receive(t.message, arrivals_->at(runner_.sample(), 1e-9 * static_cast<double>(now - t.stampNs)));

    //  The device's channels copied into the host's own buffers, made before it started: a buffer referring to
    //  this many channels would allocate. A device giving fewer plays nothing.
    if (inputCount < inputChannels_) return;
    for (int ch = 0; ch < outputChannels_; ++ch)
        if (outputs[ch] == nullptr) return;
    const auto began = juce::Time::getHighResolutionTicks();
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, in_.getNumSamples());
        for (int ch = 0; ch < inputChannels_; ++ch)
            if (inputs[ch] != nullptr)
                in_.copyFrom(ch, 0, inputs[ch] + done, n);
            else
                in_.clear(ch, 0, n);
        runner_.process(in_, out_, n);
        if (faded_ < kFadeIn) {
            const int f = static_cast<int>(std::min<std::int64_t>(n, kFadeIn - faded_));
            const float from = static_cast<float>(faded_) / kFadeIn, to = static_cast<float>(faded_ + f) / kFadeIn;
            out_.applyGainRamp(0, f, from, to);
            faded_ += f;
        }
        for (int ch = 0; ch < outputChannels_; ++ch)
            juce::FloatVectorOperations::copy(outputs[ch] + done, out_.getReadPointer(ch), n);
        done += n;
    }
    //  the block's share of its own length, smoothed over about a second
    const double took = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - began);
    const double share = took * s_.rate / static_cast<double>(frames);
    const float before = load_.load(std::memory_order_relaxed);
    const float k = static_cast<float>(std::min(1.0, static_cast<double>(frames) / s_.rate));
    load_.store(before + k * (static_cast<float>(share) - before), std::memory_order_relaxed);
}

}  // namespace bambi::livehost
