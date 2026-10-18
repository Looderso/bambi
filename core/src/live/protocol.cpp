// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/protocol.hpp"

#include <cmath>
#include <cstdlib>

namespace bambi::live {

namespace {

const char* actionName(Command::Action a) {
    switch (a) {
        case Command::Action::Play: return "play";
        case Command::Action::Stop: return "stop";
        case Command::Action::Continue: return "continue";
        case Command::Action::Locate: return "locate";
    }
    return "play";
}

const Json* member(const Json& o, std::string_view key) { return o.find(key); }

double number(const Json& o, std::string_view key, double fallback = 0.0) {
    const Json* v = member(o, key);
    return v != nullptr ? v->numberOr(fallback) : fallback;
}

std::string text(const Json& o, std::string_view key) {
    const Json* v = member(o, key);
    return v != nullptr ? std::string(v->stringOr("")) : std::string();
}

//  A flag as either side writes it: JSON true or false, or a number (the device's Max writes 0 and 1).
bool flag(const Json& o, std::string_view key) {
    const Json* v = member(o, key);
    if (v == nullptr) return false;
    return v->isBool() ? v->boolOr(false) : v->numberOr(0.0) != 0.0;
}

TrackId id(const Json& o, std::string_view key = "id") {
    const double n = number(o, key);
    return n > 0 ? static_cast<TrackId>(std::llround(n)) : 0;
}

std::optional<double> level(const Json* v) {
    return v != nullptr && v->isNumber() ? std::optional<double>(v->numberOr(0.0)) : std::nullopt;
}

Json level(const std::optional<double>& v) { return v ? Json(*v) : Json(); }

Json number(TrackId v) { return Json(static_cast<double>(v)); }

const char* kindName(Node::Kind k) {
    switch (k) {
        case Node::Kind::Group: return "group";
        case Node::Kind::Return: return "return";
        case Node::Kind::Track: break;
    }
    return "track";
}

Node node(const Json& o) {
    Node n;
    n.id = id(o);
    const std::string kind = text(o, "kind");
    n.kind = kind == "group" ? Node::Kind::Group : kind == "return" ? Node::Kind::Return : Node::Kind::Track;
    if (const Json* p = member(o, "parent"); p != nullptr && p->isNumber() && p->numberOr(0) > 0)
        n.parent = static_cast<TrackId>(std::llround(p->numberOr(0)));
    n.path = text(o, "path") == "elsewhere" ? Node::Path::Elsewhere : Node::Path::Main;
    n.name = text(o, "name");
    n.colour = text(o, "colour");
    n.audio = member(o, "audio") == nullptr || flag(o, "audio");
    if (const Json* sends = member(o, "sends"); sends != nullptr && sends->isArray())
        for (const Json& v : sends->items()) n.sends.push_back(level(&v));
    n.fader = level(member(o, "fader"));
    n.mute = flag(o, "mute");
    n.solo = flag(o, "solo");
    n.pan = number(o, "pan");
    return n;
}

Json json(const Node& n) {
    Json o = Json::object();
    o.set("id", number(n.id));
    o.set("kind", Json(kindName(n.kind)));
    o.set("parent", n.parent ? number(*n.parent) : Json());
    o.set("path", Json(n.path == Node::Path::Elsewhere ? "elsewhere" : "main"));
    o.set("name", Json(n.name));
    o.set("colour", Json(n.colour));
    o.set("audio", Json(n.audio ? 1 : 0));
    Json sends = Json::array();
    for (const auto& v : n.sends) sends.push(level(v));
    o.set("sends", std::move(sends));
    o.set("fader", level(n.fader));
    o.set("mute", Json(n.mute ? 1 : 0));
    o.set("solo", Json(n.solo ? 1 : 0));
    o.set("pan", Json(n.pan));
    return o;
}

Json typed(const char* type) {
    Json o = Json::object();
    o.set("type", Json(type));
    return o;
}

//  Each message's one way onto the wire.
struct Writer {
    Json operator()(const Hello& m) const {
        Json o = typed("hello");
        o.set("protocol", Json(m.protocol));
        o.set("live", Json(m.live));
        o.set("rate", Json(m.rate));
        return o;
    }
    Json operator()(const Switched& m) const {
        Json o = typed("bambi");
        o.set("on", Json(m.on ? 1 : 0));
        return o;
    }
    Json operator()(const Tree& m) const {
        Json o = typed("tree");
        Json nodes = Json::array();
        for (const Node& n : m.nodes) nodes.push(json(n));
        o.set("nodes", std::move(nodes));
        return o;
    }
    Json operator()(const Value& m) const {
        Json o = typed("value");
        o.set("id", number(m.id));
        o.set("name", Json(m.name));
        o.set("what", Json(m.what));
        o.set("value", m.value);
        return o;
    }
    Json operator()(const TransportMessage& m) const {
        Json o = typed("transport");
        o.set("playing", Json(m.playing ? 1 : 0));
        o.set("ppq", Json(m.ppq));
        o.set("bpm", Json(m.bpm));
        return o;
    }
    Json operator()(const Restore& m) const {
        Json o = typed("restore");
        o.set("state", m.state);
        return o;
    }
    Json operator()(const Routed& m) const {
        Json o = typed("routed");
        Json tracks = Json::array();
        for (const Routed::Track& t : m.tracks) {
            Json r = Json::object();
            r.set("id", number(t.id));
            r.set("name", Json(t.name));
            r.set("ok", Json(t.ok));
            if (t.ok)
                r.set("to", Json(t.to));
            else
                r.set("error", Json(t.error));
            tracks.push(std::move(r));
        }
        o.set("tracks", std::move(tracks));
        return o;
    }
    Json operator()(const Show&) const { return typed("show"); }
    Json operator()(const Welcome& m) const {
        Json o = typed("welcome");
        o.set("protocol", Json(m.protocol));
        o.set("loop", Json(m.loop));
        return o;
    }
    Json operator()(const Route& m) const {
        Json o = typed("route");
        Json tracks = Json::array();
        for (const TrackPair& p : m.tracks) {
            Json t = Json::object();
            t.set("id", number(p.track));
            t.set("pair", Json(p.pair));
            tracks.push(std::move(t));
        }
        o.set("tracks", std::move(tracks));
        Json silent = Json::array();
        for (const TrackId r : m.silent) silent.push(number(r));
        o.set("silent", std::move(silent));
        return o;
    }
    Json operator()(const Save& m) const {
        Json o = typed("save");
        o.set("state", m.state);
        return o;
    }
    Json operator()(const Info& m) const {
        Json o = typed("info");
        o.set("latency", Json(m.latency));
        o.set("rate", Json(m.rate));
        Json warnings = Json::object();
        for (const auto& [track, warning] : m.warnings) warnings.set(std::to_string(track), Json(warning));
        o.set("warnings", std::move(warnings));
        return o;
    }
    Json operator()(const Status& m) const {
        Json o = typed("status");
        o.set("text", Json(m.text));
        return o;
    }
    Json operator()(const Busy&) const { return typed("busy"); }
    Json operator()(const CreateReturn& m) const {
        Json o = typed("createReturn");
        o.set("name", Json(m.name));
        return o;
    }
    Json operator()(const Refresh&) const { return typed("refresh"); }
    Json operator()(const Command& m) const {
        Json o = typed("command");
        o.set("action", Json(actionName(m.action)));
        if (m.action == Command::Action::Locate) o.set("ppq", Json(m.ppq));
        return o;
    }
    Json operator()(const SlotPlay& m) const {
        Json o = typed("slot");
        o.set("id", number(m.id));
        o.set("slot", Json(m.slot));
        o.set("position", Json(m.position));
        o.set("loopStart", Json(m.loopStart));
        o.set("loopEnd", Json(m.loopEnd));
        o.set("looping", Json(m.looping));
        o.set("overrides", Json(m.overrides));
        return o;
    }
    Json operator()(const Arrangement& m) const {
        Json o = typed("arrangement");
        Json tracks = Json::array();
        for (const TrackClips& t : m.tracks) {
            Json clips = Json::array();
            for (const Clip& c : t.clips) {
                Json j = Json::object();
                j.set("start", Json(c.start));
                j.set("end", Json(c.end));
                j.set("name", Json(c.name));
                j.set("colour", Json(c.colour));
                j.set("midi", Json(c.midi));
                clips.push(std::move(j));
            }
            Json tr = Json::object();
            tr.set("id", number(t.id));
            tr.set("clips", std::move(clips));
            tracks.push(std::move(tr));
        }
        o.set("tracks", std::move(tracks));
        Json session = Json::array();
        for (const TrackSlots& t : m.session) {
            Json clips = Json::array();
            for (const SessionClip& c : t.clips) {
                Json j = Json::object();
                j.set("slot", Json(c.slot));
                j.set("name", Json(c.name));
                j.set("colour", Json(c.colour));
                j.set("loopStart", Json(c.loopStart));
                j.set("loopEnd", Json(c.loopEnd));
                j.set("looping", Json(c.looping));
                j.set("midi", Json(c.midi));
                clips.push(std::move(j));
            }
            Json tr = Json::object();
            tr.set("id", number(t.id));
            tr.set("clips", std::move(clips));
            session.push(std::move(tr));
        }
        o.set("session", std::move(session));
        o.set("scenes", Json(m.scenes));
        Json locators = Json::array();
        for (const Locator& l : m.locators) {
            Json j = Json::object();
            j.set("time", Json(l.time));
            j.set("name", Json(l.name));
            locators.push(std::move(j));
        }
        o.set("locators", std::move(locators));
        Json loop = Json::object();
        loop.set("on", Json(m.loopOn));
        loop.set("start", Json(m.loopStart));
        loop.set("length", Json(m.loopLength));
        o.set("loop", std::move(loop));
        o.set("readMs", Json(m.readMs));
        return o;
    }
};

}  // namespace

std::optional<Message> parse(std::string_view line, std::string* error) {
    std::string why;
    const Json o = Json::parse(line, &why);
    if (!o.isObject()) {
        if (error != nullptr) *error = why.empty() ? "not a JSON object" : why;
        return std::nullopt;
    }
    const std::string type = text(o, "type");
    if (type == "hello") return Hello{static_cast<int>(number(o, "protocol")), text(o, "live"), number(o, "rate")};
    if (type == "bambi") return Switched{flag(o, "on")};
    if (type == "tree") {
        Tree t;
        if (const Json* nodes = member(o, "nodes"); nodes != nullptr && nodes->isArray())
            for (const Json& n : nodes->items()) t.nodes.push_back(node(n));
        return t;
    }
    if (type == "value") {
        const Json* v = member(o, "value");
        return Value{id(o), text(o, "name"), text(o, "what"), v != nullptr ? *v : Json()};
    }
    if (type == "transport") return TransportMessage{flag(o, "playing"), number(o, "ppq"), number(o, "bpm", 120.0)};
    if (type == "restore") {
        const Json* s = member(o, "state");
        return Restore{s != nullptr ? *s : Json()};
    }
    if (type == "routed") {
        Routed r;
        if (const Json* tracks = member(o, "tracks"); tracks != nullptr && tracks->isArray())
            for (const Json& t : tracks->items())
                r.tracks.push_back({id(t), text(t, "name"), flag(t, "ok"), text(t, "to"), text(t, "error")});
        return r;
    }
    if (type == "show") return Show{};
    if (type == "welcome") return Welcome{static_cast<int>(number(o, "protocol")), flag(o, "loop")};
    if (type == "route") {
        Route r;
        if (const Json* tracks = member(o, "tracks"); tracks != nullptr && tracks->isArray())
            for (const Json& t : tracks->items())
                r.tracks.push_back({id(t), static_cast<int>(std::llround(number(t, "pair")))});
        if (const Json* silent = member(o, "silent"); silent != nullptr && silent->isArray())
            for (const Json& s : silent->items())
                if (s.numberOr(0.0) >= 1.0) r.silent.push_back(static_cast<TrackId>(std::llround(s.numberOr(0.0))));
        return r;
    }
    if (type == "save") {
        const Json* s = member(o, "state");
        return Save{s != nullptr ? *s : Json()};
    }
    if (type == "info") {
        Info i{number(o, "latency"), number(o, "rate"), {}};
        if (const Json* w = member(o, "warnings"); w != nullptr && w->isObject())
            for (const auto& [key, text] : w->members()) {
                const long long track = std::atoll(key.c_str());
                if (track > 0 && text.isString())
                    i.warnings[static_cast<TrackId>(track)] = std::string(text.stringOr(""));
            }
        return i;
    }
    if (type == "status") return Status{text(o, "text")};
    if (type == "busy") return Busy{};
    if (type == "createReturn") return CreateReturn{text(o, "name")};
    if (type == "refresh") return Refresh{};
    if (type == "command") {
        const std::string action = text(o, "action");
        const auto a = action == "stop"       ? Command::Action::Stop
                       : action == "continue" ? Command::Action::Continue
                       : action == "locate"   ? Command::Action::Locate
                       : action == "play"     ? Command::Action::Play
                                              : std::optional<Command::Action>{};
        if (!a) {
            if (error != nullptr) *error = "no transport command \"" + action + "\"";
            return std::nullopt;
        }
        return Command{*a, number(o, "ppq")};
    }
    if (type == "slot")
        return SlotPlay{id(o),
                        static_cast<int>(std::llround(number(o, "slot", -1.0))),
                        number(o, "position"),
                        number(o, "loopStart"),
                        number(o, "loopEnd"),
                        flag(o, "looping"),
                        flag(o, "overrides")};
    if (type == "arrangement") {
        Arrangement a;
        if (const Json* tracks = member(o, "tracks"); tracks != nullptr && tracks->isArray())
            for (const Json& t : tracks->items()) {
                TrackClips tc{id(t), {}};
                if (const Json* clips = member(t, "clips"); clips != nullptr && clips->isArray())
                    for (const Json& c : clips->items())
                        tc.clips.push_back({number(c, "start"), number(c, "end"), text(c, "name"), text(c, "colour"),
                                            flag(c, "midi")});
                if (tc.id != 0) a.tracks.push_back(std::move(tc));
            }
        if (const Json* session = member(o, "session"); session != nullptr && session->isArray())
            for (const Json& t : session->items()) {
                TrackSlots ts{id(t), {}};
                if (const Json* clips = member(t, "clips"); clips != nullptr && clips->isArray())
                    for (const Json& c : clips->items())
                        ts.clips.push_back({static_cast<int>(std::llround(number(c, "slot"))), text(c, "name"),
                                            text(c, "colour"), number(c, "loopStart"), number(c, "loopEnd"),
                                            flag(c, "looping"), flag(c, "midi")});
                if (ts.id != 0) a.session.push_back(std::move(ts));
            }
        a.scenes = static_cast<int>(std::llround(number(o, "scenes")));
        if (const Json* locators = member(o, "locators"); locators != nullptr && locators->isArray())
            for (const Json& l : locators->items()) a.locators.push_back({number(l, "time"), text(l, "name")});
        if (const Json* loop = member(o, "loop"); loop != nullptr && loop->isObject()) {
            a.loopOn = flag(*loop, "on");
            a.loopStart = number(*loop, "start");
            a.loopLength = number(*loop, "length");
        }
        a.readMs = number(o, "readMs");
        return a;
    }
    if (error != nullptr) *error = "no message of type \"" + type + "\"";
    return std::nullopt;
}

std::string serialise(const Message& m) { return std::visit(Writer{}, m).dump(0); }

}  // namespace bambi::live
