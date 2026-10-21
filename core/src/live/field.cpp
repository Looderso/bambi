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

//  `n` into the field under `parent`: a source when the shape takes it; a group not taken, as a bus of what is taken
//  inside it; else nothing, as it plays in Live. False when nothing of it reaches the field.
bool place(const Tree& tree, const Shape& shape, const Node& n, int parent, Field& field, int depth) {
    if (has(shape.taken, n.id)) {
        const TrackId from = shape.sidechainOf(n.id);
        field.parts.push_back({n.id, false, parent, shape.insertsOn(n.id), from == n.id ? 0 : from, {}});
        return true;
    }
    if (n.kind != Node::Kind::Group) return false;
    const int bus = static_cast<int>(field.parts.size());
    field.parts.push_back({n.id, true, parent, shape.insertsOn(n.id), 0, {}});
    bool any = false;
    //  a tree Live made has no cycle; the depth guards one a device could send
    if (depth < 64)
        for (const Node& child : tree.nodes)
            if (child.parent == n.id && child.id != n.id && reaches(child))
                any = place(tree, shape, child, bus, field, depth + 1) || any;
    if (!any) field.parts.resize(static_cast<std::size_t>(bus));  // an empty bus, and nothing under it
    return any;
}

//  What `from` is in the field: a part, by index; a return, by index; Main.
struct From {
    enum class Kind { Part, Return, Main, None } kind{Kind::None};
    int index{-1};
};

From fromOf(const Field& f, TrackId node) {
    if (node == kMain) return {From::Kind::Main, -1};
    for (std::size_t i = 0; i < f.parts.size(); ++i)
        if (f.parts[i].node == node) return {From::Kind::Part, static_cast<int>(i)};
    for (std::size_t i = 0; i < f.returns.size(); ++i)
        if (f.returns[i].node == node) return {From::Kind::Return, static_cast<int>(i)};
    return {};
}

int busOf(const Field& f, TrackId node) {
    for (std::size_t i = 0; i < f.parts.size(); ++i)
        if (f.parts[i].bus && f.parts[i].node == node) return static_cast<int>(i);
    return -1;
}

bool isReturn(const Field& f, TrackId node) {
    return std::any_of(f.returns.begin(), f.returns.end(), [&](const FieldReturn& r) { return r.node == node; });
}

bool hasDecoder(const Field& f, std::uint64_t id) {
    return std::find(f.decoders.begin(), f.decoders.end(), id) != f.decoders.end();
}

//  Whether what plays from bus `from` reaches part `to`, along the wires the field has so far.
bool busReaches(const Field& f, int from, int to, int depth = 0) {
    if (from == to) return true;
    if (depth > static_cast<int>(f.parts.size())) return true;  // a loop already: treated as reaching
    for (const Wire& r : f.parts[static_cast<std::size_t>(from)].wires)
        if (r.to.kind == Target::Kind::Node)
            if (const int b = busOf(f, r.to.id); b >= 0 && busReaches(f, b, to, depth + 1)) return true;
    return false;
}

//  Whether a wire lands on `decoder` from `t`, through whatever `t` wires on.
bool targetReaches(const Field& f, const Target& t, std::uint64_t decoder, int depth = 0) {
    if (depth > static_cast<int>(f.parts.size() + f.returns.size()) + 2) return false;
    if (t.kind == Target::Kind::Decoder) return t.id == decoder;
    const std::vector<Wire>& next = t.kind == Target::Kind::Main ? f.mainWires : f.wiresOf(t.id);
    return std::any_of(next.begin(), next.end(),
                       [&](const Wire& r) { return targetReaches(f, r.to, decoder, depth + 1); });
}

std::vector<Wire> defaultWires(const Field& f, TrackId node) {
    const From from = fromOf(f, node);
    if (from.kind == From::Kind::Main) {
        std::vector<Wire> all;
        for (const std::uint64_t d : f.decoders) all.push_back({Target::decoder(d), 0.0});
        return all;
    }
    if (from.kind == From::Kind::Part) {
        const int parent = f.parts[static_cast<std::size_t>(from.index)].parent;
        if (parent >= 0) return {{Target::node(f.parts[static_cast<std::size_t>(parent)].node), 0.0}};
    }
    return {{Target::main(), 0.0}};
}

std::vector<Wire>& wiresIn(Field& f, const From& from) {
    if (from.kind == From::Kind::Part) return f.parts[static_cast<std::size_t>(from.index)].wires;
    if (from.kind == From::Kind::Return) return f.returns[static_cast<std::size_t>(from.index)].wires;
    return f.mainWires;
}

const Routing* routingOf(const Shape& shape, TrackId node) {
    for (const Routing& r : shape.routing)
        if (r.node == node) return &r;
    return nullptr;
}

//  The shape's wires for `from`, materialised from what it plays now when it has none of its own.
std::vector<Wire>& ownWires(Shape& shape, const Field& field, TrackId from) {
    for (Routing& r : shape.routing)
        if (r.node == from) return r.wires;
    shape.routing.push_back({from, field.wiresOf(from)});
    return shape.routing.back().wires;
}

}  // namespace

float gainOf(double db) {
    if (db <= kSilentDb) return 0.0f;
    return static_cast<float>(std::pow(10.0, std::min(db, kLoudestDb) / 20.0));
}

const std::vector<Wire>& Field::wiresOf(TrackId node) const {
    static const std::vector<Wire> none;
    if (node == kMain) return mainWires;
    for (const Part& p : parts)
        if (p.node == node) return p.wires;
    for (const FieldReturn& r : returns)
        if (r.node == node) return r.wires;
    return none;
}

bool canWire(const Field& field, TrackId node, const Target& to) {
    const From from = fromOf(field, node);
    switch (from.kind) {
        case From::Kind::None: return false;
        case From::Kind::Main: return to.kind == Target::Kind::Decoder && hasDecoder(field, to.id);
        case From::Kind::Return:
            return to.kind == Target::Kind::Main || (to.kind == Target::Kind::Decoder && hasDecoder(field, to.id));
        case From::Kind::Part: break;
    }
    if (to.kind == Target::Kind::Main) return true;
    if (to.kind == Target::Kind::Decoder) return hasDecoder(field, to.id);
    if (isReturn(field, to.id)) return true;
    const int bus = busOf(field, to.id);
    return bus >= 0 && !busReaches(field, bus, from.index);
}

std::vector<Heard> heardBy(const Field& field, std::uint64_t decoder) {
    std::vector<Heard> out;
    const auto look = [&](TrackId node, const std::vector<Wire>& wires) {
        for (const Wire& r : wires) {
            if (r.to != Target::decoder(decoder)) continue;
            const bool twice = std::any_of(wires.begin(), wires.end(), [&](const Wire& o) {
                return o.to != r.to && targetReaches(field, o.to, decoder);
            });
            out.push_back({node, r.db, twice});
        }
    };
    for (const Part& p : field.parts) look(p.node, p.wires);
    for (const FieldReturn& r : field.returns) look(r.node, r.wires);
    look(kMain, field.mainWires);
    return out;
}

bool setWire(Shape& shape, const Field& field, TrackId from, const Wire& wire) {
    if (!canWire(field, from, wire.to)) return false;
    auto& wires = ownWires(shape, field, from);
    const Wire r{wire.to, std::clamp(wire.db, kSilentDb, kLoudestDb)};
    const auto at = std::find_if(wires.begin(), wires.end(), [&](const Wire& x) { return x.to == r.to; });
    if (at != wires.end())
        *at = r;
    else
        wires.push_back(r);
    return true;
}

void removeWire(Shape& shape, const Field& field, TrackId from, const Target& to) {
    auto& wires = ownWires(shape, field, from);
    std::erase_if(wires, [&](const Wire& x) { return x.to == to; });
}

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
        if (!n.parent && reaches(n)) place(tree, shape, n, -1, field, 0);
    for (const Node& n : tree.nodes)
        if (n.kind == Node::Kind::Return && has(shape.returns, n.id))
            field.returns.push_back({n.id, shape.insertsOn(n.id), {}});
    for (const HostInput& in : shape.inputs) {
        const TrackId from = shape.sidechainOf(in.id);
        field.parts.push_back({in.id, false, -1, shape.insertsOn(in.id), from == in.id ? 0 : from, {}});
    }
    field.main = shape.insertsOn(kMain);
    field.decoders = shape.decoders.empty() ? std::vector<std::uint64_t>{kFirstDecoder} : shape.decoders;
    //  the wires, node by node: the shape's, or the default; each taken only where the field can play it, so a
    //  loop is refused at the wire that would close it. The parts from the last, so what plays into a bus is wired
    //  before the bus: a bus wired into what it holds is the wire refused
    const auto take = [&](TrackId node, const From& from) {
        const Routing* own = routingOf(shape, node);
        for (const Wire& r : own != nullptr ? own->wires : defaultWires(field, node))
            if (canWire(field, node, r.to) && std::none_of(wiresIn(field, from).begin(), wiresIn(field, from).end(),
                                                           [&](const Wire& x) { return x.to == r.to; }))
                wiresIn(field, from).push_back({r.to, std::clamp(r.db, kSilentDb, kLoudestDb)});
    };
    for (std::size_t i = field.parts.size(); i-- > 0;)
        take(field.parts[i].node, {From::Kind::Part, static_cast<int>(i)});
    for (std::size_t i = 0; i < field.returns.size(); ++i)
        take(field.returns[i].node, {From::Kind::Return, static_cast<int>(i)});
    take(kMain, {From::Kind::Main, -1});
    return field;
}

void takeAll(Shape& shape, const Tree& tree) {
    //  a group that is a bus is not taken whole, which would silence what it holds: what it holds not yet taken is
    const auto holdsTaken = [&](const auto& self, TrackId group, int depth) -> bool {
        for (const Node& c : tree.nodes)
            if (c.parent == group && c.id != group &&
                (has(shape.taken, c.id) || (depth < 64 && self(self, c.id, depth + 1))))
                return true;
        return false;
    };
    const auto take = [&](const auto& self, const Node& n, int depth) -> void {
        if (has(shape.taken, n.id)) return;
        if (n.kind == Node::Kind::Group && depth < 64 && holdsTaken(holdsTaken, n.id, 0)) {
            for (const Node& c : tree.nodes)
                if (c.parent == n.id && c.id != n.id && reaches(c)) self(self, c, depth + 1);
            return;
        }
        shape.taken.push_back(n.id);
    };
    for (const Node& n : tree.nodes)
        if (!n.parent && reaches(n)) take(take, n, 0);
}

void adoptSplit(Shape& shape, const Tree& tree) {
    if (shape.split.empty()) return;
    std::vector<TrackId> taken;
    const auto put = [&](TrackId id) {
        if (!has(taken, id)) taken.push_back(id);
    };
    //  as such a set laid it out: a split group's children that reach it, each a source or split again
    const auto open = [&](const auto& self, const Node& n, int depth) -> void {
        if (n.kind != Node::Kind::Group || !has(shape.split, n.id) || depth >= 64) {
            put(n.id);
            return;
        }
        for (const Node& child : tree.nodes)
            if (child.parent == n.id && child.id != n.id && reaches(child)) self(self, child, depth + 1);
    };
    for (const TrackId id : shape.taken) {
        const auto n = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& x) { return x.id == id; });
        if (n == tree.nodes.end() || n->parent)
            put(id);
        else
            open(open, *n, 0);
    }
    shape.taken = std::move(taken);
    shape.split.clear();
}

void moveInsert(Shape& shape, TrackId owner, std::uint64_t id, std::size_t to) {
    for (Chain& c : shape.chains) {
        if (c.node != owner) continue;
        const auto at = std::find_if(c.inserts.begin(), c.inserts.end(), [&](const Insert& i) { return i.id == id; });
        if (at == c.inserts.end()) return;
        const Insert moving = *at;
        c.inserts.erase(at);
        c.inserts.insert(c.inserts.begin() + static_cast<std::ptrdiff_t>(std::min(to, c.inserts.size())), moving);
        return;
    }
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
    std::vector<std::size_t> partOf;                  // each node's part
    std::vector<const std::vector<Insert>*> chains;   // each node's inserts, slotted once the Encoders are counted
    for (std::size_t i = 0; i < field.parts.size(); ++i) {
        const Part& part = field.parts[i];
        FieldNode n;
        n.track = part.node;
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
        partOf.push_back(i);
        chains.push_back(&part.inserts);
    }
    //  the wires, once every node has its place: a bus, a return, a Decoder by its index in the plan; a wire to
    //  something the plan does not have is left out, with its level
    const auto planned = [&](const std::vector<Wire>& wires) {
        std::vector<PlanWire> to;
        for (const Wire& w : wires) {
            std::optional<PlanWire> pw;
            if (w.to.kind == Target::Kind::Main) {
                pw = PlanWire{PlanWire::To::Main, 0};
            } else if (w.to.kind == Target::Kind::Decoder) {
                const auto d = std::find(field.decoders.begin(), field.decoders.end(), w.to.id);
                if (d != field.decoders.end())
                    pw = PlanWire{PlanWire::To::Decoder, static_cast<int>(d - field.decoders.begin())};
            } else if (const int bus = busOf(field, w.to.id); bus >= 0 && nodeOf[static_cast<std::size_t>(bus)] >= 0) {
                pw = PlanWire{PlanWire::To::Node, nodeOf[static_cast<std::size_t>(bus)]};
            } else {
                const auto r = std::find_if(field.returns.begin(), field.returns.end(),
                                            [&](const FieldReturn& x) { return x.node == w.to.id; });
                if (r != field.returns.end())
                    pw = PlanWire{PlanWire::To::Return, static_cast<int>(r - field.returns.begin())};
            }
            if (pw) to.push_back(*pw);
        }
        return to;
    };
    for (std::size_t k = 0; k < out.plan.nodes.size(); ++k)
        out.plan.nodes[k].wires = planned(field.parts[partOf[k]].wires);
    const auto slot = [&](const Insert& insert) {
        out.effects.push_back(insert);
        return static_cast<int>(out.encoders.size() + out.effects.size()) - 1;
    };
    for (std::size_t k = 0; k < out.plan.nodes.size(); ++k)
        for (const Insert& insert : *chains[k]) out.plan.nodes[k].inserts.push_back(slot(insert));
    for (const FieldReturn& r : field.returns) {
        Return ret;
        for (const Insert& insert : r.inserts) ret.chain.push_back(slot(insert));
        ret.wires = planned(r.wires);
        out.plan.returns.push_back(std::move(ret));
    }
    out.plan.mainWires = planned(field.mainWires);
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
    const auto nodeOf = [&](TrackId id) -> const Node* {
        const auto n = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const Node& x) { return x.id == id; });
        return n == tree.nodes.end() ? nullptr : &*n;
    };
    const auto fader = [](const Node* n) {
        return n == nullptr || !n->fader ? 0.0f : static_cast<float>(std::pow(10.0, *n->fader / 20.0));
    };
    const std::vector<WireOf> wires = wiresOf(plan.plan);
    std::vector<float> out(wires.size(), 0.0f);
    for (std::size_t e = 0; e < wires.size(); ++e) {
        const WireOf& w = wires[e];
        const TrackId from = w.from == WireOf::From::Node     ? plan.plan.nodes[static_cast<std::size_t>(w.index)].track
                             : w.from == WireOf::From::Return ? field.returns[static_cast<std::size_t>(w.index)].node
                                                              : kMain;
        const auto i = static_cast<std::size_t>(w.wire.index);
        const Target to = w.wire.to == PlanWire::To::Node     ? Target::node(plan.plan.nodes[i].track)
                          : w.wire.to == PlanWire::To::Return ? Target::node(field.returns[i].node)
                          : w.wire.to == PlanWire::To::Main   ? Target::main()
                                                              : Target::decoder(plan.decoders[i]);
        const std::vector<Wire>& own = field.wiresOf(from);
        const auto wire = std::find_if(own.begin(), own.end(), [&](const Wire& x) { return x.to == to; });
        if (wire == own.end()) continue;  // a plan older than the field: silent until it is laid out again
        float live = 1.0f;                // a source at unity, Main too
        if (w.from == WireOf::From::Node && plan.plan.nodes[static_cast<std::size_t>(w.index)].input < 0) {
            const Node* group = nodeOf(from);  // a bus at its group's fader, silent while muted
            live = group == nullptr || group->mute ? 0.0f : fader(group);
        } else if (w.from == WireOf::From::Return) {
            live = fader(nodeOf(from));
        }
        out[e] = gainOf(wire->db) * live;
    }
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
    if (!shape.split.empty()) j.set("split", ids(shape.split));  // an older set's, until a tree adopts it
    j.set("taken", ids(shape.taken));
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
    Json routing = Json::array();
    for (const Routing& r : shape.routing) {
        Json wires = Json::array();
        for (const Wire& wire : r.wires) {
            Json o = Json::object();
            o.set("to", Json(wire.to.kind == Target::Kind::Node   ? "node"
                             : wire.to.kind == Target::Kind::Main ? "main"
                                                                  : "decoder"));
            if (wire.to.kind != Target::Kind::Main) o.set("id", Json(static_cast<double>(wire.to.id)));
            o.set("db", Json(wire.db));
            wires.push(std::move(o));
        }
        Json o = Json::object();
        o.set("node", Json(static_cast<double>(r.node)));
        o.set("wires", std::move(wires));
        routing.push(std::move(o));
    }
    j.set("routing", std::move(routing));
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
    Shape shape{ids("split"), ids("taken"), {}, ids("returns"), {}, {}, {}, {}};
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
    if (const Json* list = json.find("routing"); list != nullptr && list->isArray())
        for (const Json& o : list->items()) {
            const Json* node = o.find("node");
            const auto at = node != nullptr ? idOf(*node, 0.0) : std::nullopt;
            const Json* wires = o.find("wires");
            if (!at || wires == nullptr || !wires->isArray() || routingOf(shape, *at) != nullptr) continue;
            Routing r{*at, {}};
            for (const Json& wire : wires->items()) {
                const Json* to = wire.find("to");
                const std::string_view kind = to != nullptr ? to->stringOr("") : std::string_view();
                const Json* id = wire.find("id");
                const auto target = id != nullptr ? idOf(*id, 1.0) : std::nullopt;
                const Json* db = wire.find("db");
                const double level = std::clamp(db != nullptr ? db->numberOr(0.0) : 0.0, kSilentDb, kLoudestDb);
                if (kind == "main")
                    r.wires.push_back({Target::main(), level});
                else if (kind == "node" && target)
                    r.wires.push_back({Target::node(*target), level});
                else if (kind == "decoder" && target)
                    r.wires.push_back({Target::decoder(*target), level});
            }
            shape.routing.push_back(std::move(r));
        }
    return shape;
}

}  // namespace bambi::live
