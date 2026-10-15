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
        field.parts.push_back({n.id, false, parent, shape.insertsOn(n.id)});
        return true;
    }
    const int bus = static_cast<int>(field.parts.size());
    field.parts.push_back({n.id, true, parent, shape.insertsOn(n.id)});
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
    field.main = shape.insertsOn(kMain);
    return field;
}

FieldPlan planOf(const Field& field, const std::vector<TrackPair>& pairs) {
    FieldPlan out;
    std::vector<int> nodeOf(field.parts.size(), -1);  // a part's node in the plan; -1: left out
    std::vector<const std::vector<Insert>*> chains;   // each node's inserts, slotted once the Encoders are counted
    for (std::size_t i = 0; i < field.parts.size(); ++i) {
        const Part& part = field.parts[i];
        FieldNode n;
        n.track = part.node;
        n.parent = part.parent < 0 ? -1 : nodeOf[static_cast<std::size_t>(part.parent)];
        if (!part.bus) {
            const auto p =
                std::find_if(pairs.begin(), pairs.end(), [&](const TrackPair& tp) { return tp.track == part.node; });
            if (p == pairs.end()) continue;
            n.pair = p->pair;
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
    out.plan.decoder = static_cast<int>(out.encoders.size() + out.effects.size());
    out.plan.slots.assign(static_cast<std::size_t>(out.plan.decoder) + 1, Slot{});
    return out;
}

std::vector<float> sendLevels(const FieldPlan& plan, const Field& field, const Tree& tree) {
    const std::size_t returns = field.returns.size();
    std::vector<float> levels(plan.plan.nodes.size() * returns, 0.0f);
    for (std::size_t k = 0; k < plan.plan.nodes.size(); ++k) {
        const auto node = std::find_if(tree.nodes.begin(), tree.nodes.end(),
                                       [&](const Node& n) { return n.id == plan.plan.nodes[k].track; });
        if (node == tree.nodes.end()) continue;
        for (std::size_t r = 0; r < returns; ++r) {
            const auto send = static_cast<std::size_t>(field.returns[r].send);
            if (send < node->sends.size() && node->sends[send])
                levels[k * returns + r] = static_cast<float>(std::pow(10.0, *node->sends[send] / 20.0));
        }
    }
    return levels;
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
    Shape shape{ids("split"), ids("inLive"), {}, ids("returns")};
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
                    if (n && e) chain.inserts.push_back({*n, *e});
                }
            shape.chains.push_back(std::move(chain));
        }
    return shape;
}

}  // namespace bambi::live
