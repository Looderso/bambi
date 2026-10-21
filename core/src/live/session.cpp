// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/session.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <variant>

#include "bambi/live/page.hpp"

namespace bambi::live {

namespace {

//  A value the device observed, into the node it belongs to; one it does not know is left alone.
void take(Node& n, const Value& v) {
    if (v.what == "name")
        n.name = std::string(v.value.stringOr(n.name));
    else if (v.what == "colour")
        n.colour = std::string(v.value.stringOr(n.colour));
    else if (v.what == "mute")
        n.mute = v.value.numberOr(0.0) != 0.0;
    else if (v.what == "solo")
        n.solo = v.value.numberOr(0.0) != 0.0;
    else if (v.what == "pan")
        n.pan = v.value.numberOr(n.pan);
    else if (v.what == "fader")
        n.fader = v.value.isNumber() ? std::optional<double>(v.value.numberOr(0.0)) : std::nullopt;
    else if (v.what.rfind("send:", 0) == 0) {
        long index = -1;
        const std::string_view digits = std::string_view(v.what).substr(5);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec != std::errc{} || index < 0) return;
        if (static_cast<std::size_t>(index) >= n.sends.size()) n.sends.resize(static_cast<std::size_t>(index) + 1);
        n.sends[static_cast<std::size_t>(index)] =
            v.value.isNumber() ? std::optional<double>(v.value.numberOr(0.0)) : std::nullopt;
    }
}

}  // namespace

Json DeviceSession::state() const {
    Json pairs = Json::array();
    for (const TrackPair& p : pairs_) {
        Json t = Json::object();
        t.set("id", Json(static_cast<double>(p.track)));
        t.set("pair", Json(p.pair));
        pairs.push(std::move(t));
    }
    Json state = kept_;
    state.set("pairs", std::move(pairs));
    state.set("shape", toJson(shape_));
    return state;
}

void DeviceSession::keep(std::string_view name, Json value) { kept_.set(name, std::move(value)); }

const Node* DeviceSession::trackOn(int pair) const {
    for (const TrackPair& p : pairs_)
        if (p.pair == pair)
            for (const Node& n : tree_.nodes)
                if (n.id == p.track) return &n;
    return nullptr;
}

std::vector<Message> DeviceSession::onTree(const Tree& t, bool fromLive) {
    adoptSplit(shape_, t);
    //  a track the field plays, filed by Live into a group the field encodes whole: the group opens, as an Encoder
    //  added there opens it, so the track keeps its Encoder and inserts
    if (fromLive && treed_)
        for (const TrackId s : field_.sources())
            if (std::any_of(t.nodes.begin(), t.nodes.end(), [&](const Node& n) { return n.id == s; }) &&
                roleOf(t, layOut(t, shape_), s) == Role::InGroup)
                addEncoder(shape_, t, s);
    tree_ = t;
    treed_ = true;
    field_ = layOut(tree_, shape_);
    const std::vector<TrackId> sources = field_.sources();
    const PairAssignment a = assignPairs(pairs_, sources, s_.pairCount);
    pairs_ = a.pairs;
    unassigned_ = a.unassigned;
    std::vector<TrackId> silent;
    silent.reserve(field_.returns.size());
    for (const FieldReturn& r : field_.returns) silent.push_back(r.node);
    return {Route{pairs_, std::move(silent)}, Save{state()}, Info{s_.latencyMs, s_.rate, {}}};
}

std::vector<Message> DeviceSession::reshape(Shape shape) {
    shape_ = std::move(shape);
    if (!treed_) return {};
    return onTree(Tree{tree_}, false);
}

std::vector<Message> DeviceSession::answer(const Message& in) {
    if (const auto* h = std::get_if<Hello>(&in)) {
        hello_ = *h;
        return {Welcome{kProtocol, s_.loop}};
    }
    //  another protocol's messages mean something else: not answered, and nothing taken from them
    if (!compatible()) return {};
    if (const auto* t = std::get_if<Tree>(&in)) return onTree(*t, true);
    if (const auto* a = std::get_if<Arrangement>(&in)) {
        arrangement_ = *a;
        return {};
    }
    if (const auto* r = std::get_if<Restore>(&in)) {
        pairs_.clear();
        kept_ = Json::object();
        if (r->state.isObject())
            for (const auto& [name, value] : r->state.members()) keep(name, value);
        if (const Json* shape = r->state.find("shape")) shape_ = shapeFrom(*shape);
        if (const Json* pairs = r->state.find("pairs"); pairs != nullptr && pairs->isArray())
            for (const Json& p : pairs->items()) {
                const Json* id = p.find("id");
                const Json* pair = p.find("pair");
                if (id != nullptr && pair != nullptr && id->numberOr(0) > 0)
                    pairs_.push_back({static_cast<TrackId>(std::llround(id->numberOr(0))),
                                      static_cast<int>(std::llround(pair->numberOr(-1)))});
            }
        return {};
    }
    if (const auto* b = std::get_if<Switched>(&in)) {
        on_ = b->on;
        return {};
    }
    if (const auto* v = std::get_if<Value>(&in)) {
        for (Node& n : tree_.nodes)
            if (n.id == v->id) take(n, *v);
        return {};
    }
    if (const auto* m = std::get_if<TransportMessage>(&in)) {
        transport_ = *m;
        return {};
    }
    if (const auto* r = std::get_if<Routed>(&in)) {
        routed_ = *r;
        return {};
    }
    if (std::holds_alternative<Show>(in)) ++shows_;
    return {};
}

std::vector<std::string> DeviceSession::channelNames() const {
    std::vector<std::string> names(static_cast<std::size_t>(2 * (s_.pairCount + 1)));
    for (const TrackPair& p : pairs_) {
        if (p.pair < 0 || p.pair >= s_.pairCount) continue;
        const auto node =
            std::find_if(tree_.nodes.begin(), tree_.nodes.end(), [&](const Node& n) { return n.id == p.track; });
        if (node == tree_.nodes.end()) continue;
        names[static_cast<std::size_t>(kFirstTrackChannel + 2 * p.pair)] = node->name + " L";
        names[static_cast<std::size_t>(kFirstTrackChannel + 2 * p.pair + 1)] = node->name + " R";
    }
    names[0] = "main L";
    names[1] = "main R";
    return names;
}

void DeviceSession::disconnected() {
    hello_.reset();
    on_ = false;
    transport_.reset();
    routed_ = {};
}

}  // namespace bambi::live
