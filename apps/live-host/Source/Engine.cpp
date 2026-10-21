// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Plugins.h"
#include "bambi/decode/remote.hpp"
#include "bambi/live/graph.hpp"
#include "bambi/live/timeline.hpp"
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
        const int to = r.to == live::OutputRoute::To::Row ? loopChannels_ + r.dest - 1 : mainChannel_ + r.dest - 1;
        if (to >= 0 && to < outputChannels_)
            out.push_back({r.from == live::OutputRoute::From::Main, r.decoder, r.channel - 1, to, r.delay});
    }
    return out;
}

Engine::~Engine() {
    midiWatch_ = {};
    midiInputs_.clear();
    port_.reset();
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
    uids.reserve(joined.size());
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
    opened_ = std::move(opened);
    //  the callback's buffers, while no audio runs: a device may hand over more than its buffer at once, played in
    //  pieces of these
    const int piece = std::max(block_, 1024);
    inAt_.assign(static_cast<std::size_t>(inputChannels_), nullptr);
    out_.setSize(outputChannels_, piece);
    outputLatency_ = device->getOutputLatencyInSamples() + block_;
    deviceLatency_ = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() + block_;
    return true;
}

Runner::Setup Engine::runnerSetup() const {
    return {s_.order, block_, inputChannels_, outputChannels_, s_.rate, mainChannel_, static_cast<int>(s_.rate)};
}

bool Engine::start(const Settings& settings, juce::String& status) {
    s_ = settings;
    mainChannel_ = 0;
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
    if (!compiled ||
        !runner_.prepare(*compiled, {first.get()}, compiled->levels, runnerSetup(), why, {first}, routes_)) {
        status = "the first program: " + (compiled ? why : juce::String(error));
        return false;
    }
    program_ = {*compiled, {first.get()}, compiled->levels, {first}};
    projector_ = live::TransportProjector({s_.rate});
    locates_.store(0, std::memory_order_release);
    arrivals_.emplace(s_.rate, s_.transportOffsetSamples);
    runner_.setClock(&projector_);
    runner_.setClips(&clips_);
    runner_.setMidi(&pending_);
    liveMidi_.store(std::llround(s_.audio.liveMidiMs * 0.001 * s_.rate), std::memory_order_relaxed);
    port_ = juce::MidiInput::createNewDevice("bambi", this);
    livePort_ = port_.get();
    if (port_ != nullptr) port_->start();
    openMidi();
    midiWatch_ = juce::MidiDeviceListConnection::make([this] { openMidi(); });
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
    liveMidi_.store(std::llround(s_.audio.liveMidiMs * 0.001 * s_.rate), std::memory_order_relaxed);
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
    //  the runner counts from 0 again: what waits was placed on the samples before, and is let go of, its notes with
    //  it; the meter starts afresh. No audio runs
    pending_.clear();
    silence_ = true;
    wasMeasuring_ = false;
    liveMidi_.store(std::llround(s_.audio.liveMidiMs * 0.001 * s_.rate), std::memory_order_relaxed);
    latencyMs_ = 1000.0 * (deviceLatency_ + program_.compiled.latency) / s_.rate;
    devices_.addAudioCallback(this);
    status = said();
    return true;
}

void Engine::tellDecoders() {
    const auto decoders = playing();
    DecoderRequest request;
    HostOutputs host;
    for (const live::AudioDevice& d : row_.devices)
        if (d.outs > 0) host.row.push_back({d.name, d.outs});
    for (const live::PlayingDecoder& d : decoders) {
        auto* remote = [&]() -> remote::IRemote* {
            for (const Dec& dec : decoders_)
                if (dec.id == d.id) return remoteOf(*dec.plugin, kDecoderRemoteKind, kDecoderRemoteVersion);
            return nullptr;
        }();
        if (remote == nullptr) continue;
        host.headphones =
            live::takenBy({d.id, Output::Headphones}, s_.audio.at({d.id, Output::Headphones}), row_, decoders);
        host.speakers = live::takenBy({d.id, Output::Speakers}, s_.audio.at({d.id, Output::Speakers}), row_, decoders);
        request.host = host;
        std::string text = toJson(request).dump(0);
        const void* instance = remote;
        if (auto& told = told_[d.id]; told.first != instance || told.second != text) {
            told = {instance, text};
            remote->remoteRequest(text.c_str());
        }
    }
}

std::vector<live::Placed> Engine::placementsAsked() {
    std::vector<live::Placed> out;
    for (const live::PlayingDecoder& d : playing()) {
        int& served = served_[d.id];
        if (d.report.placeSerial == served) continue;
        served = d.report.placeSerial;
        const live::DeviceChannel at = row_.channel(d.report.placeAt);
        if (at.none() || (d.report.placeOut != 1 && d.report.placeOut != 2)) continue;
        out.push_back({{d.id, d.report.placeOut == 1 ? Output::Headphones : Output::Speakers}, at});
    }
    return out;
}

bool Engine::setOrder(int order, juce::String& status) {
    if (order == s_.order) return true;
    //  every state as it is, for the plugins loaded again at the new order
    const Json kept = states();
    devices_.removeAudioCallback(this);
    voices_.clear();
    effects_.clear();
    decoders_.clear();
    playing_.clear();
    plan_ = {};
    fieldPlan_ = {};
    s_.order = order;
    auto first = loadDecoder(status);
    if (first == nullptr) return false;
    decoders_ = {{live::kFirstDecoder, first}};
    live::Plan plan;
    plan.slots = {{first->getLatencySamples()}};
    plan.decoders = {0};
    std::string error;
    const auto compiled = live::compile(plan, &error);
    juce::String why;
    if (compiled) routes_ = routesFor(compiled->latency);
    if (!compiled ||
        !runner_.prepare(*compiled, {first.get()}, compiled->levels, runnerSetup(), why, {first}, routes_)) {
        status = "the first program: " + (compiled ? why : juce::String(error));
        return false;
    }
    program_ = {*compiled, {first.get()}, compiled->levels, {first}};
    pending_.clear();
    restore(kept);  // the first Decoder takes its own; the rest wait for the field
    latencyMs_ = 1000.0 * (deviceLatency_ + compiled->latency) / s_.rate;
    devices_.addAudioCallback(this);
    status = said();
    return true;
}

bool Engine::reroute() {
    if (program_.slots.empty()) return false;
    tellDecoders();
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
    for (const live::Arrival& a : live::inputArrivals(s_.audio, inputs, inputRow_, loopChannels_))
        arrivals.push_back(a);
    live::FieldPlan fp = live::planOf(field, arrivals);
    //  the restored states the new plugins took: given up only once the swap is made, so a change that fails part
    //  of the way leaves them for the next
    std::vector<std::string> restoredEncoders, restoredEffects;
    //  each source's Encoder: the one it had, or one loaded now
    std::vector<Voice> next;
    std::vector<int> drop;  // nodes whose Encoder could not be loaded
    for (std::size_t slot = 0; slot < fp.encoders.size(); ++slot) {
        const live::TrackId track = fp.encoders[slot];
        const auto on =
            std::find_if(pairs.begin(), pairs.end(), [&](const live::TrackPair& p) { return p.track == track; });
        const int pair = on == pairs.end() ? -1 : on->pair;  // an input of the host has none
        const auto had = std::find_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.track == track; });
        if (had != voices_.end()) {
            next.push_back({track, pair, had->encoder});
            continue;
        }
        auto loaded = loadInstalled("bambi Path Encoder", Role::Encoder, s_.order, s_.rate, block_);
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
        out.reserve(list.size());
        for (const Dec& d : list) out.push_back(d.id);
        return out;
    };
    const auto sidechains = [](const live::Plan& p) {
        std::vector<int> out;
        out.reserve(p.slots.size());
        for (const live::Slot& slot : p.slots) out.push_back(slot.sidechain);
        return out;
    };
    if (fp.plan.nodes == plan_.nodes && fp.plan.main == plan_.main && fp.effects == playing_ &&
        ids(nextDecoders) == ids(decoders_) && sidechains(fp.plan) == sidechains(plan_) &&
        fp.plan.returns == plan_.returns && fp.plan.mainWires == plan_.mainWires)
        return false;  // the same shape, wires and plugins: a level alone is setSends()'s

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
    drive();  // the lanes onto the instances that play now, and the nodes they are on
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

void Engine::slot(const live::SlotPlay& report, std::int64_t stampNs) noexcept { (void)slots_.push({report, stampNs}); }

Engine::Target Engine::targetOf(live::Instance instance, std::uint64_t owner, std::string_view param) const {
    Target t;
    if (instance == live::Instance::Encoder) {
        for (const Voice& v : voices_)
            if (v.track == owner) t = {&manifestOf(instance), -1, v.encoder.get()};
    } else if (instance == live::Instance::Insert) {
        for (const Fx& f : effects_)
            if (f.insert.id == owner) t = {&manifestOf(instance, f.insert.effect), -1, f.plugin.get()};
    } else {
        for (const Dec& d : decoders_)
            if (d.id == owner) t = {&manifestOf(instance), -1, d.plugin.get()};
    }
    if (t.plugin == nullptr) return {};
    t.at = t.manifest->byKey(param);
    //  the hosted parameter at the manifest's place, and by the manifest's name: a plugin that lists them otherwise
    //  is driven nowhere rather than on the wrong one
    const auto& params = t.plugin->getParameters();
    if (t.at < 0 || t.at >= params.size() ||
        params[t.at]->getName(256) != juce::String(std::string((*t.manifest)[t.at].name)))
        return {};
    return t;
}

void Engine::setLanes(std::vector<live::Lane> lanes) {
    lanes_ = std::move(lanes);
    drive();
}

void Engine::drive() {
    std::vector<Runner::Driven> driven;
    Runner::Keep keep;
    const auto alive = [&](juce::AudioProcessor* p) -> std::shared_ptr<void> {
        for (const Voice& v : voices_)
            if (v.encoder.get() == p) return v.encoder;
        for (const Fx& f : effects_)
            if (f.plugin.get() == p) return f.plugin;
        for (const Dec& d : decoders_)
            if (d.plugin.get() == p) return d.plugin;
        return nullptr;
    };
    for (const live::Lane& lane : lanes_) {
        if (lane.points.empty()) continue;
        const Target t = targetOf(lane.instance, lane.owner, lane.param);
        if (t.plugin == nullptr) continue;
        std::vector<live::LanePoint> points;
        points.reserve(lane.points.size());
        for (const live::LanePoint& p : lane.points)
            points.push_back(
                {p.ppq, toNormalised(*t.manifest, t.at, clampToRange(*t.manifest, t.at, static_cast<float>(p.value)))});
        //  a song lane waits while its track plays a session clip: an Encoder's track, or the node an insert is on
        live::TrackId heldBy = lane.instance == live::Instance::Encoder ? lane.owner : 0;
        if (lane.instance == live::Instance::Insert)
            for (const live::FieldNode& n : plan_.nodes)
                for (const int s : n.inserts) {
                    const auto e = static_cast<std::size_t>(s) - voices_.size();
                    if (s >= static_cast<int>(voices_.size()) && e < effects_.size() &&
                        effects_[e].insert.id == lane.owner)
                        heldBy = n.track;
                }
        driven.push_back(
            {t.plugin->getParameters()[t.at], std::move(points), lane.clipTrack, lane.slot, lane.clip() ? 0 : heldBy});
        keep.push_back(alive(t.plugin));
    }
    runner_.setAutomation(std::move(driven), std::move(keep));
}

live::Playhead Engine::heard() const noexcept {
    live::Playhead p = runner_.shown();
    const int late = program_.compiled.latency + outputLatency_;
    if (p.playing) p.ppq = live::heardAt(p.ppq, p.bpm, s_.rate, late);
    return p;
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

// ---- MIDI --------------------------------------------------------------------------------------

void Engine::openMidi() {
    const auto available = juce::MidiInput::getAvailableDevices();
    //  the ones gone let go of, the ones new opened; one already open stays as it is
    std::erase_if(midiInputs_, [&](const std::unique_ptr<juce::MidiInput>& in) {
        return std::none_of(available.begin(), available.end(),
                            [&](const juce::MidiDeviceInfo& d) { return d.identifier == in->getIdentifier(); });
    });
    for (const juce::MidiDeviceInfo& d : available) {
        if (port_ != nullptr && d.identifier == port_->getIdentifier()) continue;
        if (std::any_of(midiInputs_.begin(), midiInputs_.end(),
                        [&](const auto& in) { return in->getIdentifier() == d.identifier; }))
            continue;
        if (auto in = juce::MidiInput::openDevice(d.identifier, this)) {
            in->start();
            midiInputs_.push_back(std::move(in));
        }
    }
}

std::vector<juce::String> Engine::midiInputs() const {
    std::vector<juce::String> out;
    out.reserve(midiInputs_.size());
    for (const auto& in : midiInputs_) out.push_back(in->getName());
    if (port_ != nullptr) out.push_back(port_->getName());
    return out;
}

void Engine::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) {
    const int size = message.getRawDataSize();
    if (size < 1 || size > 3 || message.isSysEx()) return;
    TimedMidi m;
    std::copy_n(message.getRawData(), size, m.bytes.begin());
    m.size = static_cast<std::uint8_t>(size);
    m.live = source == livePort_;
    //  its stamp, on JUCE's millisecond counter, as an age: ahead when the sender scheduled it, and taken as now
    //  when far off -- a sender that stamps 0 for "now"
    const double ago = 0.001 * juce::Time::getMillisecondCounterHiRes() - message.getTimeStamp();
    m.stampNs = nowNs() - (std::abs(ago) < 0.5 ? static_cast<std::int64_t>(ago * 1e9) : 0);
    const juce::SpinLock::ScopedLockType one(midiPush_);
    (void)midi_.push(m);
}

void Engine::takeMidi(const float* const* inputs, int inputCount, int frames, std::int64_t now) noexcept {
    const std::int64_t start = runner_.sample();
    const bool measuring = measuring_.load(std::memory_order_relaxed);
    if (measuring && !wasMeasuring_) meter_.emplace(s_.rate);
    wasMeasuring_ = measuring;
    for (TimedMidi m; midi_.pop(m);) {
        //  where it arrived, on the host's samples: in the past, or ahead when the sender scheduled it
        const std::int64_t arrived = start - std::llround(1e-9 * static_cast<double>(now - m.stampNs) * s_.rate);
        if (m.live && meter_ && measuring && m.size == 3 && (m.bytes[0] & 0xf0) == 0x90 && m.bytes[2] > 0)
            meter_->note(arrived);
        const std::int64_t at = arrived + (m.live ? liveMidi_.load(std::memory_order_relaxed) : block_);
        if (!pending_.add({std::max(at, start), m.bytes, m.size})) silence_ = true;
    }
    if (silence_) {
        //  a note-off may have been lost: all of them let go, on every channel, before anything else
        pending_.clear();
        for (std::uint8_t channel = 0; channel < 16; ++channel)
            (void)pending_.add({start, {static_cast<std::uint8_t>(0xb0 | channel), 123, 0}, 3});
        silence_ = false;
    }
    if (!measuring || !meter_) return;
    const auto channel = [&](int c) { return c < inputCount ? inputs[c] : nullptr; };
    meter_->listen(channel(mainChannel_), channel(mainChannel_ + 1), frames, start);
    if (const auto lead = meter_->took()) (void)leads_.push(*lead);
}

std::optional<double> Engine::measured() noexcept {
    std::int64_t lead = 0;
    if (!leads_.pop(lead)) return std::nullopt;
    return 1000.0 * static_cast<double>(lead) / s_.rate;
}

void Engine::audioDeviceStopped() { playable_.store(false, std::memory_order_relaxed); }

void Engine::audioDeviceIOCallbackWithContext(const float* const* inputs, int inputCount, float* const* outputs,
                                              int outputCount, int frames, const juce::AudioIODeviceCallbackContext&) {
    for (int ch = 0; ch < outputCount; ++ch)
        if (outputs[ch] != nullptr) juce::FloatVectorOperations::clear(outputs[ch], frames);
    if (!playable_.load(std::memory_order_relaxed) || outputCount < outputChannels_ || !arrivals_) return;

    live::Arrivals& arrivals = *arrivals_;
    const std::int64_t now = nowNs();
    for (Timed t; queue_.pop(t);)
        projector_.receive(t.message, arrivals.at(runner_.sample(), 1e-9 * static_cast<double>(now - t.stampNs)));
    if (projector_.locates() != locates_.load(std::memory_order_relaxed)) {
        lastLocateSeconds_.store(projector_.lastLocateSeconds(), std::memory_order_relaxed);
        locates_.store(projector_.locates(), std::memory_order_release);
    }
    for (TimedSlot t; slots_.pop(t);)
        clips_.receive(t.report, arrivals.at(runner_.sample(), 1e-9 * static_cast<double>(now - t.stampNs)));

    //  The output copied out of the host's own buffer, made before it started: a buffer referring to this many
    //  channels would allocate. A device giving fewer plays nothing.
    if (inputCount < inputChannels_) return;
    for (int ch = 0; ch < outputChannels_; ++ch)
        if (outputs[ch] == nullptr) return;
    takeMidi(inputs, inputCount, frames, now);
    const auto began = juce::Time::getHighResolutionTicks();
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, out_.getNumSamples());
        //  the device's inputs read where they are: pointers to this piece of each, made before audio ran
        for (int ch = 0; ch < inputChannels_; ++ch)
            inAt_[static_cast<std::size_t>(ch)] = inputs[ch] != nullptr ? inputs[ch] + done : nullptr;
        runner_.process(Runner::Inputs{inAt_.data(), inputChannels_}, out_, n);
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
