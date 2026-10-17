// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/io.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bambi::live {

namespace {
int channelsOf(const AudioDevice& d, bool inputs) { return inputs ? d.ins : d.outs; }
}  // namespace

int Row::width() const {
    int n = 0;
    for (const AudioDevice& d : devices) n += channelsOf(d, inputs);
    return n;
}

int Row::at(const DeviceChannel& c) const {
    int offset = 0;
    for (const AudioDevice& d : devices) {
        if (d.uid == c.device) return c.number >= 1 && c.number <= channelsOf(d, inputs) ? offset + c.number : 0;
        offset += channelsOf(d, inputs);
    }
    return 0;
}

DeviceChannel Row::channel(int at) const {
    int offset = 0;
    for (const AudioDevice& d : devices) {
        if (at > offset && at <= offset + channelsOf(d, inputs)) return {d.uid, at - offset};
        offset += channelsOf(d, inputs);
    }
    return {};
}

int Row::deviceOf(int at) const {
    int offset = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (at > offset && at <= offset + channelsOf(devices[i], inputs)) return static_cast<int>(i);
        offset += channelsOf(devices[i], inputs);
    }
    return -1;
}

std::vector<AudioDevice> aggregateOf(const std::vector<std::string>& chosen, const std::vector<AudioDevice>& system) {
    std::vector<AudioDevice> out;
    int outs = 0;
    for (const std::string& uid : chosen) {
        const auto d = std::find_if(system.begin(), system.end(), [&](const AudioDevice& x) { return x.uid == uid; });
        if (d == system.end() || d->wireless) continue;
        if (std::any_of(out.begin(), out.end(), [&](const AudioDevice& x) { return x.uid == uid; })) continue;
        if (outs + d->outs > kMaxRowChannels) break;
        outs += d->outs;
        out.push_back(*d);
    }
    return out;
}

Row rowOf(const std::vector<AudioDevice>& devices, bool inputs) { return {devices, inputs}; }

namespace {

Json channelJson(const DeviceChannel& c) {
    Json j = Json::object();
    j.set("device", Json(c.device));
    j.set("number", Json(c.number));
    return j;
}

DeviceChannel channelFrom(const Json* j) {
    if (j == nullptr || !j->isObject()) return {};
    DeviceChannel c;
    if (const Json* d = j->find("device")) c.device = std::string(d->stringOr(""));
    if (const Json* n = j->find("number")) c.number = std::max(0, static_cast<int>(std::lround(n->numberOr(0.0))));
    return c;
}

bool boolAt(const Json& j, const char* key, bool fallback) {
    const Json* v = j.find(key);
    return v != nullptr ? v->boolOr(fallback) : fallback;
}

//  An output as the file names it: its Decoder's id, 0 for Main, and which of its outputs.
void outInto(Json& j, const Out& o) {
    j.set("decoder", Json(static_cast<double>(o.decoder)));
    if (!o.main()) j.set("output", Json(outputName(o.which)));
}
Out outFrom(const Json& j) {
    //  a file from when there was one Decoder: "of" names the output, Main among them
    if (const Json* of = j.find("of"); of != nullptr && of->isString()) {
        const std::string_view name = of->stringOr("");
        if (name == "main") return kLiveMain;
        return {kFirstDecoder, name == "speakers" ? Output::Speakers : Output::Headphones};
    }
    Out o;
    if (const Json* d = j.find("decoder"); d != nullptr && d->numberOr(0.0) >= 1.0)
        o.decoder = static_cast<std::uint64_t>(std::llround(d->numberOr(0.0)));
    if (const Json* w = j.find("output"))
        o.which = w->stringOr("") == "speakers" ? Output::Speakers : Output::Headphones;
    return o;
}

const PlayingDecoder* decoderOf(std::uint64_t id, const std::vector<PlayingDecoder>& decoders) {
    const auto d = std::find_if(decoders.begin(), decoders.end(), [&](const PlayingDecoder& x) { return x.id == id; });
    return d == decoders.end() ? nullptr : &*d;
}

}  // namespace

DeviceChannel AudioSettings::at(const Out& out) const {
    if (out.main()) return main;
    for (const Placed& p : placed)
        if (p.out == out) return p.at;
    return {};
}

Json toJson(const AudioSettings& s) {
    Json j = Json::object();
    Json devices = Json::array();
    for (const std::string& d : s.devices) devices.push(Json(d));
    j.set("devices", std::move(devices));
    j.set("buffer", Json(s.buffer));
    j.set("main", channelJson(s.main));
    Json placed = Json::array();
    for (const Placed& p : s.placed) {
        Json o = Json::object();
        outInto(o, p.out);
        o.set("at", channelJson(p.at));
        placed.push(std::move(o));
    }
    j.set("placed", std::move(placed));
    Json copies = Json::array();
    for (const Copy& c : s.copies) {
        Json o = Json::object();
        outInto(o, c.of);
        o.set("at", channelJson(c.at));
        o.set("toLive", Json(c.toLive));
        copies.push(std::move(o));
    }
    j.set("copies", std::move(copies));
    Json inputs = Json::array();
    for (const AudioSettings::InputAt& in : s.inputs) {
        Json o = Json::object();
        o.set("input", Json(static_cast<double>(in.input)));
        o.set("at", channelJson(in.at));
        inputs.push(std::move(o));
    }
    j.set("inputs", std::move(inputs));
    return j;
}

AudioSettings audioSettingsFrom(const Json& j) {
    AudioSettings s;
    if (const Json* list = j.find("devices"); list != nullptr && list->isArray())
        for (const Json& d : list->items())
            if (d.isString()) s.devices.emplace_back(d.stringOr(""));
    if (const Json* b = j.find("buffer")) s.buffer = std::max(1, static_cast<int>(std::lround(b->numberOr(64.0))));
    s.main = channelFrom(j.find("main"));
    //  a file from when there was one Decoder: its outputs' places
    for (const Output which : {Output::Headphones, Output::Speakers})
        if (const DeviceChannel at = channelFrom(j.find(outputName(which))); !at.none())
            s.placed.push_back({{kFirstDecoder, which}, at});
    if (const Json* list = j.find("placed"); list != nullptr && list->isArray())
        for (const Json& o : list->items())
            if (const Out out = outFrom(o); !out.main()) s.placed.push_back({out, channelFrom(o.find("at"))});
    if (const Json* list = j.find("copies"); list != nullptr && list->isArray())
        for (const Json& o : list->items())
            s.copies.push_back({outFrom(o), channelFrom(o.find("at")), boolAt(o, "toLive", false)});
    if (const Json* list = j.find("inputs"); list != nullptr && list->isArray())
        for (const Json& o : list->items()) {
            const Json* id = o.find("input");
            const double v = id != nullptr ? id->numberOr(0.0) : 0.0;
            if (v >= static_cast<double>(kFirstInput))
                s.inputs.push_back({static_cast<TrackId>(std::llround(v)), channelFrom(o.find("at"))});
        }
    return s;
}

std::vector<int> busChannels(const Out& out, const std::vector<PlayingDecoder>& decoders) {
    if (out.main()) return {1, 2};
    const PlayingDecoder* d = decoderOf(out.decoder, decoders);
    if (d == nullptr) return {};
    const DecoderReport& r = d->report;
    if (out.which == Output::Headphones)
        return r.headphonesOn ? std::vector<int>{r.headphoneChannel, r.headphoneChannel + 1} : std::vector<int>{};
    return r.speakersOn ? r.speakerChannels : std::vector<int>{};
}

std::vector<int> takenBy(const Out& out, const DeviceChannel& at, const Row& row,
                         const std::vector<PlayingDecoder>& decoders) {
    const int start = row.at(at);
    const std::vector<int> bus = busChannels(out, decoders);
    if (start == 0 || bus.empty()) return {};
    std::vector<int> taken;
    for (const int c : bus)
        if (const int x = c - bus.front() + start; x >= 1 && x <= row.width()) taken.push_back(x);
    return taken;
}

std::vector<Arrival> inputArrivals(const AudioSettings& s, const std::vector<HostInput>& inputs, const Row& row,
                                   int loopChannels) {
    std::vector<Arrival> out;
    for (const HostInput& in : inputs) {
        const auto kept = std::find_if(s.inputs.begin(), s.inputs.end(),
                                       [&](const AudioSettings::InputAt& x) { return x.input == in.id; });
        if (kept == s.inputs.end()) continue;
        const int first = row.at(kept->at);
        const int width = in.width == 2 ? 2 : 1;
        if (first == 0 || row.deviceOf(first + width - 1) != row.deviceOf(first)) continue;
        out.push_back({in.id, loopChannels + first - 1, width});
    }
    return out;
}

OutputPlan planOutputs(const AudioSettings& s, const Row& row, const std::vector<PlayingDecoder>& decoders,
                       int fieldLatency) {
    OutputPlan plan;
    int slowest = 0, slowestDecoder = 0;
    for (const AudioDevice& d : row.devices) slowest = std::max(slowest, d.outLatency);
    for (const PlayingDecoder& d : decoders) slowestDecoder = std::max(slowestDecoder, d.latency);
    const auto align = [&](int at) {
        const int i = row.deviceOf(at);
        return i < 0 ? 0 : slowest - row.devices[static_cast<std::size_t>(i)].outLatency;
    };
    //  `out`'s channels onto `dests` or the loop device's pair; Main late by the field, a quicker Decoder held back
    const auto add = [&](const Out& out, OutputRoute::To to, const std::vector<int>& dests) {
        const std::vector<int> sources = busChannels(out, decoders);
        int index = 0, late = fieldLatency;
        if (!out.main()) {
            const auto d = std::find_if(decoders.begin(), decoders.end(),
                                        [&](const PlayingDecoder& x) { return x.id == out.decoder; });
            if (d == decoders.end()) return;
            index = static_cast<int>(d - decoders.begin());
            late = slowestDecoder - d->latency;
        }
        const auto from = out.main() ? OutputRoute::From::Main : OutputRoute::From::Decoder;
        for (std::size_t i = 0; i < sources.size() && i < dests.size(); ++i) {
            const int dest = dests[i];
            if (to == OutputRoute::To::Row && (dest < 1 || dest > row.width())) continue;
            plan.routes.push_back(
                {from, index, sources[i], to, dest, late + (to == OutputRoute::To::Row ? align(dest) : 0)});
        }
    };

    for (const PlayingDecoder& d : decoders)
        for (const Output which : {Output::Headphones, Output::Speakers}) {
            const Out out{d.id, which};
            add(out, OutputRoute::To::Row, takenBy(out, s.at(out), row, decoders));
        }
    add(kLiveMain, OutputRoute::To::Row, takenBy(kLiveMain, s.main, row, decoders));
    for (const Copy& c : s.copies) {
        //  back to Live: a pair, and never Main, which Live has
        if (c.toLive) {
            if (!c.of.main() && busChannels(c.of, decoders).size() == 2) add(c.of, OutputRoute::To::Loop, {1, 2});
            continue;
        }
        add(c.of, OutputRoute::To::Row, takenBy(c.of, c.at, row, decoders));
    }

    std::vector<int> reached;
    for (const OutputRoute& r : plan.routes)
        if (r.to == OutputRoute::To::Row) reached.push_back(r.dest);
    std::sort(reached.begin(), reached.end());
    for (std::size_t i = 1; i < reached.size(); ++i)
        if (reached[i] == reached[i - 1] && (plan.clashes.empty() || plan.clashes.back() != reached[i]))
            plan.clashes.push_back(reached[i]);
    return plan;
}

}  // namespace bambi::live
