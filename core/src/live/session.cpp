// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <variant>

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
        const long index = std::atol(v.what.c_str() + 5);
        if (index < 0) return;
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
    Json state = Json::object();
    state.set("pairs", std::move(pairs));
    return state;
}

std::vector<Message> DeviceSession::onTree(const Tree& t) {
    tree_ = t;
    std::vector<TrackId> top;
    for (const Node& n : t.nodes)
        if (!n.parent && n.path == Node::Path::Main && n.kind != Node::Kind::Return && n.audio) top.push_back(n.id);
    const PairAssignment a = assignPairs(pairs_, top, s_.pairCount);
    pairs_ = a.pairs;
    unassigned_ = a.unassigned;
    return {Route{pairs_}, Save{state()}, Info{s_.latencyMs, s_.rate, {}}};
}

std::vector<Message> DeviceSession::answer(const Message& in) {
    if (const auto* h = std::get_if<Hello>(&in)) {
        hello_ = *h;
        return {Welcome{kProtocol, s_.loop}};
    }
    if (const auto* t = std::get_if<Tree>(&in)) return onTree(*t);
    if (const auto* r = std::get_if<Restore>(&in)) {
        pairs_.clear();
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
        names[static_cast<std::size_t>(2 * p.pair)] = node->name + " L";
        names[static_cast<std::size_t>(2 * p.pair + 1)] = node->name + " R";
    }
    names[static_cast<std::size_t>(2 * s_.pairCount)] = "bambi return L";
    names[static_cast<std::size_t>(2 * s_.pairCount + 1)] = "bambi return R";
    return names;
}

void DeviceSession::disconnected() {
    hello_.reset();
    on_ = false;
    transport_.reset();
    routed_ = {};
}

}  // namespace bambi::live
