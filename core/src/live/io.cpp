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

std::vector<int> Row::deviceEnds() const {
    std::vector<int> ends;
    int offset = 0, last = 0;
    for (const AudioDevice& d : devices) last += channelsOf(d, inputs);
    for (const AudioDevice& d : devices) {
        if (channelsOf(d, inputs) == 0) continue;
        offset += channelsOf(d, inputs);
        if (offset < last) ends.push_back(offset);
    }
    return ends;
}

OutputBus Row::bus(std::vector<int> kept) const {
    OutputBus b(width());
    b.kept = std::move(kept);
    b.deviceEnds = deviceEnds();
    return b;
}

std::vector<AudioDevice> aggregateOf(const std::vector<std::string>& chosen, const std::vector<AudioDevice>& system) {
    std::vector<AudioDevice> out;
    int outs = 0;
    for (const std::string& uid : chosen) {
        const auto d = std::find_if(system.begin(), system.end(), [&](const AudioDevice& x) { return x.uid == uid; });
        if (d == system.end() || d->bluetooth) continue;
        if (std::any_of(out.begin(), out.end(), [&](const AudioDevice& x) { return x.uid == uid; })) continue;
        if (outs + d->outs > kMaxRowChannels) break;
        outs += d->outs;
        out.push_back(*d);
    }
    return out;
}

Row rowOf(const std::vector<AudioDevice>& devices, bool inputs) { return {devices, inputs}; }

namespace {

const char* signalName(Signal s) {
    return s == Signal::Main ? "main" : s == Signal::Headphones ? "headphones" : "speakers";
}

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

//  The Decoder's channels a signal plays on, as it reports them; Main's are its own two.
std::vector<int> sourceChannels(Signal s, const DecoderReport& d) {
    if (s == Signal::Main) return {1, 2};
    if (s == Signal::Headphones) return d.headphonesOn ? std::vector<int>{d.headphoneChannel, d.headphoneChannel + 1}
                                                       : std::vector<int>{};
    return d.speakersOn ? d.speakerChannels : std::vector<int>{};
}

//  The row channels starting at `start` that `channels` take, moved as a block from their first.
std::vector<int> moved(const std::vector<int>& channels, int from, int start) {
    std::vector<int> out;
    for (const int c : channels) out.push_back(c - from + start);
    return out;
}

}  // namespace

Json toJson(const AudioSettings& s) {
    Json j = Json::object();
    Json devices = Json::array();
    for (const std::string& d : s.devices) devices.push(Json(d));
    j.set("devices", std::move(devices));
    j.set("buffer", Json(s.buffer));
    j.set("main", channelJson(s.main));
    j.set("headphonesOn", Json(s.headphonesOn));
    j.set("headphones", channelJson(s.headphones));
    j.set("speakersOn", Json(s.speakersOn));
    j.set("speakers", channelJson(s.speakers));
    j.set("layout", Json(s.layout));
    j.set("levelDb", Json(s.levelDb));
    Json copies = Json::array();
    for (const Copy& c : s.copies) {
        Json o = Json::object();
        o.set("of", Json(signalName(c.of)));
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
    s.headphonesOn = boolAt(j, "headphonesOn", s.headphonesOn);
    s.headphones = channelFrom(j.find("headphones"));
    s.speakersOn = boolAt(j, "speakersOn", s.speakersOn);
    s.speakers = channelFrom(j.find("speakers"));
    if (const Json* l = j.find("layout")) s.layout = std::string(l->stringOr(""));
    if (const Json* v = j.find("levelDb")) s.levelDb = v->numberOr(0.0);
    if (const Json* list = j.find("copies"); list != nullptr && list->isArray())
        for (const Json& o : list->items()) {
            const Json* of = o.find("of");
            const std::string_view name = of != nullptr ? of->stringOr("") : "";
            const auto signal = name == "main"         ? std::optional<Signal>(Signal::Main)
                                : name == "headphones" ? std::optional<Signal>(Signal::Headphones)
                                : name == "speakers"   ? std::optional<Signal>(Signal::Speakers)
                                                       : std::nullopt;
            if (signal) s.copies.push_back({*signal, channelFrom(o.find("at")), boolAt(o, "toLive", false)});
        }
    if (const Json* list = j.find("inputs"); list != nullptr && list->isArray())
        for (const Json& o : list->items()) {
            const Json* id = o.find("input");
            const double v = id != nullptr ? id->numberOr(0.0) : 0.0;
            if (v >= static_cast<double>(kFirstInput))
                s.inputs.push_back({static_cast<TrackId>(std::llround(v)), channelFrom(o.find("at"))});
        }
    return s;
}

std::vector<int> takenBy(const Copy& c, const Row& row, const DecoderReport& decoder) {
    const int start = row.at(c.at);
    if (c.toLive || start == 0) return {};
    std::vector<int> out;
    if (c.of != Signal::Speakers)
        out = {start, start + 1};
    else
        out = moved(decoder.speakerChannels, decoder.speakerFirstChannel, start);
    out.erase(std::remove_if(out.begin(), out.end(), [&](int x) { return x < 1 || x > row.width(); }), out.end());
    return out;
}

std::vector<int> keptChannels(const AudioSettings& s, const Row& row, const DecoderReport& decoder) {
    std::vector<int> kept = takenBy({Signal::Main, s.main, false}, row, decoder);
    for (const Copy& c : s.copies)
        for (const int x : takenBy(c, row, decoder)) kept.push_back(x);
    std::sort(kept.begin(), kept.end());
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    return kept;
}

DecoderRequest requestFor(const AudioSettings& s, const Row& row, const DecoderReport& decoder) {
    DecoderRequest r;
    //  an output whose device has gone stops, rather than play on whatever is now where it was
    const int hp = row.at(s.headphones), sp = row.at(s.speakers);
    r.headphonesOn = s.headphonesOn && hp != 0;
    if (hp != 0) r.headphoneChannel = hp;
    r.speakersOn = s.speakersOn && sp != 0;
    if (sp != 0) r.speakerFirstChannel = sp;
    if (!s.layout.empty()) r.layout = s.layout;
    r.levelDb = s.levelDb;
    r.kept = keptChannels(s, row, decoder);
    r.deviceEnds = row.deviceEnds();
    return r;
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

OutputPlan planOutputs(const AudioSettings& s, const Row& row, const DecoderReport& decoder, int fieldLatency) {
    OutputPlan plan;
    int slowest = 0;
    for (const AudioDevice& d : row.devices) slowest = std::max(slowest, d.outLatency);
    const auto align = [&](int at) {
        const int i = row.deviceOf(at);
        return i < 0 ? 0 : slowest - row.devices[static_cast<std::size_t>(i)].outLatency;
    };
    const auto add = [&](OutputRoute::From from, const std::vector<int>& sources, OutputRoute::To to,
                         const std::vector<int>& dests) {
        for (std::size_t i = 0; i < sources.size() && i < dests.size(); ++i) {
            const int dest = dests[i];
            if (to == OutputRoute::To::Row && (dest < 1 || dest > row.width())) continue;
            const int late = from == OutputRoute::From::Main ? fieldLatency : 0;
            plan.routes.push_back({from, sources[i], to, dest, late + (to == OutputRoute::To::Row ? align(dest) : 0)});
        }
    };

    for (const Signal out : {Signal::Headphones, Signal::Speakers}) {
        const std::vector<int> channels = sourceChannels(out, decoder);
        add(OutputRoute::From::Decoder, channels, OutputRoute::To::Row, channels);
    }
    add(OutputRoute::From::Main, {1, 2}, OutputRoute::To::Row, takenBy({Signal::Main, s.main, false}, row, decoder));
    for (const Copy& c : s.copies) {
        const std::vector<int> sources = sourceChannels(c.of, decoder);
        const auto from = c.of == Signal::Main ? OutputRoute::From::Main : OutputRoute::From::Decoder;
        //  back to Live: a pair, and never Main, which Live has
        if (c.toLive) {
            if (c.of != Signal::Main && sources.size() == 2) add(from, sources, OutputRoute::To::Loop, {1, 2});
            continue;
        }
        add(from, sources, OutputRoute::To::Row, takenBy(c, row, decoder));
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
