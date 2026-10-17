// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/field.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace bambi::live {

namespace {

bool has(const std::vector<TrackId>& ids, TrackId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

bool reaches(const Node& n) { return n.path == Node::Path::Main && n.kind != Node::Kind::Return && n.audio; }

//  `n` into the field under `parent`. False when nothing of it reaches the field.
bool place(const Tree& tree, const Shape& shape, const Node& n, int parent, Field& field, int depth) {
    const bool split = n.kind == Node::Kind::Group && has(shape.split, n.id);
    if (!split) {
        const TrackId from = shape.sidechainOf(n.id);
        field.parts.push_back({n.id, false, parent, shape.insertsOn(n.id), from == n.id ? 0 : from});
        return true;
    }
    const int bus = static_cast<int>(field.parts.size());
    field.parts.push_back({n.id, true, parent, shape.insertsOn(n.id), 0});
    bool any = false;
    //  a tree Live made has no cycle; the depth guards one a device could send
    if (depth < 64)
        for (const Node& child : tree.nodes)
            if (child.parent == n.id && child.id != n.id && reaches(child))
                any = place(tree, shape, child, bus, field, depth + 1) || any;
    if (!any) field.parts.resize(static_cast<std::size_t>(bus));  // an empty bus, and nothing under it
    return any;
}

}  // namespace

const std::vector<Insert>& Shape::insertsOn(TrackId node) const {
    static const std::vector<Insert> none;
    for (const Chain& c : chains)
        if (c.node == node) return c.inserts;
    return none;
}

TrackId Shape::sidechainOf(TrackId source) const {
    for (const Sidechain& c : sidechains)
        if (c.source == source) return c.from;
    return 0;
}

std::vector<TrackId> Field::sources() const {
    std::vector<TrackId> out;
    for (const Part& p : parts)
        if (!p.bus) out.push_back(p.node);
    return out;
}

Field layOut(const Tree& tree, const Shape& shape) {
    Field field;
    for (const Node& n : tree.nodes)
        if (!n.parent && reaches(n) && !has(shape.inLive, n.id)) place(tree, shape, n, -1, field, 0);
    int send = 0;
    for (const Node& n : tree.nodes) {
        if (n.kind != Node::Kind::Return) continue;
        if (has(shape.returns, n.id)) field.returns.push_back({n.id, send, shape.insertsOn(n.id)});
        ++send;
    }
    for (const HostInput& in : shape.inputs) {
        const TrackId from = shape.sidechainOf(in.id);
        field.parts.push_back({in.id, false, -1, shape.insertsOn(in.id), from == in.id ? 0 : from});
    }
    field.main = shape.insertsOn(kMain);
    field.decoders = shape.decoders.empty() ? std::vector<std::uint64_t>{kFirstDecoder} : shape.decoders;
    return field;
}

std::vector<Arrival> arrivalsOf(const std::vector<TrackPair>& pairs) {
    std::vector<Arrival> out;
    for (const TrackPair& p : pairs) out.push_back({p.track, kFirstTrackChannel + 2 * p.pair, 2});
    return out;
}

FieldPlan planOf(const Field& field, const std::vector<Arrival>& arrivals) {
    const auto arrivalOf = [&](TrackId t) {
        const auto a = std::find_if(arrivals.begin(), arrivals.end(), [&](const Arrival& x) { return x.track == t; });
        return a == arrivals.end() ? nullptr : &*a;
    };
    FieldPlan out;
    std::vector<int> nodeOf(field.parts.size(), -1);  // a part's node in the plan; -1: left out
    std::vector<const std::vector<Insert>*> chains;   // each node's inserts, slotted once the Encoders are counted
    for (std::size_t i = 0; i < field.parts.size(); ++i) {
        const Part& part = field.parts[i];
        FieldNode n;
        n.track = part.node;
        n.parent = part.parent < 0 ? -1 : nodeOf[static_cast<std::size_t>(part.parent)];
        if (!part.bus) {
            const Arrival* a = arrivalOf(part.node);
            if (a == nullptr) continue;
            n.input = a->channel;
            n.width = a->width;
            n.encoder = static_cast<int>(out.encoders.size());
            out.encoders.push_back(part.node);
        }
        nodeOf[i] = static_cast<int>(out.plan.nodes.size());
        out.plan.nodes.push_back(n);
        chains.push_back(&part.inserts);
    }
    const auto slot = [&](const Insert& insert) {
        out.effects.push_back(insert);
        return static_cast<int>(out.encoders.size() + out.effects.size()) - 1;
    };
    for (std::size_t k = 0; k < out.plan.nodes.size(); ++k)
        for (const Insert& insert : *chains[k]) out.plan.nodes[k].inserts.push_back(slot(insert));
    for (const FieldReturn& r : field.returns) {
        Return ret;
        for (const Insert& insert : r.inserts) ret.chain.push_back(slot(insert));
        out.plan.returns.push_back(std::move(ret));
    }
    for (const Insert& insert : field.main) out.plan.main.push_back(slot(insert));
    for (const std::uint64_t d : field.decoders) {
        out.plan.decoders.push_back(static_cast<int>(out.encoders.size() + out.effects.size() + out.decoders.size()));
        out.decoders.push_back(d);
    }
    out.plan.slots.assign(out.encoders.size() + out.effects.size() + out.decoders.size(), Slot{});
    //  only the field's sources arrive -- a bus, a node in Live, a track inside a whole group, no track (0) none
    const auto hear = [&](Slot& slot, TrackId from) {
        if (const Arrival* a = from != 0 ? arrivalOf(from) : nullptr) {
            slot.sidechain = a->channel;
            slot.sidechainWidth = a->width;
        }
    };
    for (const FieldNode& n : out.plan.nodes)
        if (n.encoder >= 0) {
            const auto part =
                std::find_if(field.parts.begin(), field.parts.end(), [&](const Part& x) { return x.node == n.track; });
            hear(out.plan.slots[static_cast<std::size_t>(n.encoder)], part->sidechain);
        }
    for (std::size_t e = 0; e < out.effects.size(); ++e)
        hear(out.plan.slots[out.encoders.size() + e], out.effects[e].sidechain);
    return out;
}

std::vector<float> levels(const FieldPlan& plan, const Field& field, const Tree& tree) {
    const std::size_t nodes = plan.plan.nodes.size(), returns = field.returns.size();
    std::vector<float> out(nodes * returns + nodes + returns, 0.0f);
    const auto nodeOf = [&](TrackId id) -> const Node* {
        const auto n = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& x) { return x.id == id; });
        return n == tree.nodes.end() ? nullptr : &*n;
    };
    const auto gain = [](const std::optional<double>& dB) {
        return dB ? static_cast<float>(std::pow(10.0, *dB / 20.0)) : 0.0f;
    };
    for (std::size_t k = 0; k < nodes; ++k) {
        const FieldNode& fn = plan.plan.nodes[k];
        const Node* node = nodeOf(fn.track);
        const bool bus = fn.input < 0;
        //  a bus at its group's fader, silent while muted; a source at unity
        const float fader = !bus ? 1.0f : node == nullptr || node->mute ? 0.0f : gain(node->fader);
        out[nodes * returns + k] = fader;
        if (node == nullptr) continue;
        for (std::size_t r = 0; r < returns; ++r) {
            const auto send = static_cast<std::size_t>(field.returns[r].send);
            if (send < node->sends.size()) out[k * returns + r] = gain(node->sends[send]) * (bus ? fader : 1.0f);
        }
    }
    for (std::size_t r = 0; r < returns; ++r)
        if (const Node* ret = nodeOf(field.returns[r].node)) out[nodes * returns + nodes + r] = gain(ret->fader);
    return out;
}

namespace {

const char* nameOf(Effect e) { return e == Effect::Reverb ? "reverb" : "echo"; }

std::optional<Effect> effectNamed(std::string_view name) {
    if (name == "echo") return Effect::Echo;
    if (name == "reverb") return Effect::Reverb;
    return std::nullopt;
}

std::optional<TrackId> idOf(const Json& j, double least) {
    const double v = j.numberOr(-1.0);
    if (!j.isNumber() || v < least) return std::nullopt;
    return static_cast<TrackId>(std::llround(v));
}

}  // namespace

Json toJson(const Shape& shape) {
    const auto ids = [](const std::vector<TrackId>& list) {
        Json a = Json::array();
        for (const TrackId id : list) a.push(Json(static_cast<double>(id)));
        return a;
    };
    Json chains = Json::array();
    for (const Chain& c : shape.chains) {
        Json inserts = Json::array();
        for (const Insert& i : c.inserts) {
            Json insert = Json::object();
            insert.set("id", Json(static_cast<double>(i.id)));
            insert.set("effect", Json(std::string(nameOf(i.effect))));
            if (i.sidechain != 0) insert.set("sidechain", Json(static_cast<double>(i.sidechain)));
            inserts.push(std::move(insert));
        }
        Json chain = Json::object();
        chain.set("node", Json(static_cast<double>(c.node)));
        chain.set("inserts", std::move(inserts));
        chains.push(std::move(chain));
    }
    Json j = Json::object();
    j.set("split", ids(shape.split));
    j.set("inLive", ids(shape.inLive));
    j.set("chains", std::move(chains));
    j.set("returns", ids(shape.returns));
    Json sidechains = Json::array();
    for (const Sidechain& c : shape.sidechains) {
        Json o = Json::object();
        o.set("source", Json(static_cast<double>(c.source)));
        o.set("from", Json(static_cast<double>(c.from)));
        sidechains.push(std::move(o));
    }
    j.set("sidechains", std::move(sidechains));
    Json inputs = Json::array();
    for (const HostInput& in : shape.inputs) {
        Json o = Json::object();
        o.set("id", Json(static_cast<double>(in.id)));
        o.set("name", Json(in.name));
        o.set("width", Json(in.width));
        inputs.push(std::move(o));
    }
    j.set("inputs", std::move(inputs));
    if (!shape.decoders.empty()) j.set("decoders", ids(shape.decoders));
    return j;
}

Shape shapeFrom(const Json& json) {
    const auto ids = [&](const char* key) {
        std::vector<TrackId> out;
        if (const Json* list = json.find(key); list != nullptr && list->isArray())
            for (const Json& id : list->items())
                if (const auto v = idOf(id, 1.0)) out.push_back(*v);
        return out;
    };
    Shape shape{ids("split"), ids("inLive"), {}, ids("returns"), {}, {}, {}};
    if (const Json* list = json.find("sidechains"); list != nullptr && list->isArray())
        for (const Json& c : list->items()) {
            const Json* source = c.find("source");
            const Json* from = c.find("from");
            const auto a = source != nullptr ? idOf(*source, 1.0) : std::nullopt;
            const auto b = from != nullptr ? idOf(*from, 1.0) : std::nullopt;
            if (a && b) shape.sidechains.push_back({*a, *b});
        }
    if (const Json* chains = json.find("chains"); chains != nullptr && chains->isArray())
        for (const Json& c : chains->items()) {
            const Json* node = c.find("node");
            const auto at = node != nullptr ? idOf(*node, 0.0) : std::nullopt;
            if (!at) continue;
            Chain chain{*at, {}};
            if (const Json* inserts = c.find("inserts"); inserts != nullptr && inserts->isArray())
                for (const Json& i : inserts->items()) {
                    const Json* id = i.find("id");
                    const Json* effect = i.find("effect");
                    const auto n = id != nullptr ? idOf(*id, 1.0) : std::nullopt;
                    const auto e = effect != nullptr ? effectNamed(effect->stringOr("")) : std::nullopt;
                    const Json* from = i.find("sidechain");
                    const auto side = from != nullptr ? idOf(*from, 1.0) : std::nullopt;
                    if (n && e) chain.inserts.push_back({*n, *e, side.value_or(0)});
                }
            shape.chains.push_back(std::move(chain));
        }
    shape.decoders = ids("decoders");
    if (const Json* inputs = json.find("inputs"); inputs != nullptr && inputs->isArray())
        for (const Json& o : inputs->items()) {
            const Json* id = o.find("id");
            const auto at = id != nullptr ? idOf(*id, static_cast<double>(kFirstInput)) : std::nullopt;
            const Json* name = o.find("name");
            const Json* width = o.find("width");
            if (at)
                shape.inputs.push_back({*at, name != nullptr ? std::string(name->stringOr("")) : std::string(),
                                        width != nullptr && width->numberOr(1.0) >= 2.0 ? 2 : 1});
        }
    return shape;
}

}  // namespace bambi::live
