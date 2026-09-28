// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/state.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

#include "bambi/path/authoring.hpp"
#include "bambi/path/trajectory.hpp"

namespace bambi {
namespace {

/// Written out rather than a defaulted operator== so that Vec3 need not gain one -- exact
/// equality on a vector is a thing worth having to ask for.
bool isDefaultTrajectory(const TrajectoryState& t) {
    const TrajectoryState d;
    if (t.kind != d.kind || t.closed != d.closed || t.generator != d.generator || !t.nodes.empty()) return false;
    for (std::size_t i = 0; i < t.genParams.size(); ++i)
        if (t.genParams[i] != d.genParams[i]) return false;
    return true;
}

template <typename T, std::size_t N>
T parseEnum(std::string_view s, const std::array<std::string_view, N>& names, T def) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == s) return static_cast<T>(i);
    return def;  // unknown name: fall back rather than fail — a newer build may have added one
}

constexpr std::array<std::string_view, 2> kKindNames{"parametric", "custom"};
constexpr std::array<std::string_view, 5> kGenNames{"orbit", "lissajous", "wave", "arc", "spiral"};
constexpr std::array<std::string_view, 3> kInputNames{"self", "sidechain", "link"};
constexpr std::array<std::string_view, 4> kTabNames{"features", "sidechain", "generators", "region"};
constexpr std::array<std::string_view, 2> kTriggerInputNames{"midi", "audio"};
constexpr std::array<std::string_view, 2> kTriggerGateNames{"held", "one-shot"};
constexpr std::array<std::string_view, 7> kRegionKindNames{"everywhere", "spot",   "band",  "sectors",
                                                           "dots",       "clouds", "custom"};
constexpr std::array<std::string_view, 2> kRegionSideNames{"inside", "outside"};
constexpr std::array<std::string_view, 2> kRenderQualityNames{"same", "realistic"};

constexpr std::array<std::string_view, 3> kOrbitParams{"tilt", "spin", "aperture"};
constexpr std::array<std::string_view, 5> kLissParams{"az_amount", "el_amount", "az_ratio", "el_ratio", "phase"};
constexpr std::array<std::string_view, 4> kWaveParams{"turns", "el_amount", "wobbles", "el_offset"};
constexpr std::array<std::string_view, 4> kArcParams{"centre_az", "centre_el", "length", "heading"};
constexpr std::array<std::string_view, 4> kSpiralParams{"turns", "from_el", "to_el", "start_az"};

Json vecToJson(Vec3 v) {
    Json a = Json::array();
    a.push(Json{v.x});
    a.push(Json{v.y});
    a.push(Json{v.z});
    return a;
}

Vec3 vecFromJson(const Json* j, Vec3 def) {
    if (j == nullptr || !j->isArray() || j->size() != 3) return def;
    return {j->items()[0].numberOr(def.x), j->items()[1].numberOr(def.y), j->items()[2].numberOr(def.z)};
}

/// A slot number spelled out in full, or -1: "3" is 3; "", "x" and "3x" are -1.
long slotNumber(std::string_view text) {
    long n = -1;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
    return error == std::errc{} && end == text.data() + text.size() && n >= 0 ? n : -1;
}

}  // namespace

std::string_view name(TrajectoryKind v) { return kKindNames[static_cast<std::size_t>(v)]; }
std::string_view name(GeneratorType v) { return kGenNames[static_cast<std::size_t>(v)]; }
std::string_view name(SourceInput v) { return kInputNames[static_cast<std::size_t>(v)]; }
std::string_view name(MatrixTab v) { return kTabNames[static_cast<std::size_t>(v)]; }
std::string_view name(TriggerInput v) { return kTriggerInputNames[static_cast<std::size_t>(v)]; }
std::string_view name(TriggerGate v) { return kTriggerGateNames[static_cast<std::size_t>(v)]; }
std::string_view name(RegionKind v) { return kRegionKindNames[static_cast<std::size_t>(v)]; }
std::string_view name(RegionSide v) { return kRegionSideNames[static_cast<std::size_t>(v)]; }
std::string_view name(RenderQuality v) { return kRenderQualityNames[static_cast<std::size_t>(v)]; }

std::span<const std::string_view> generatorParamNames(GeneratorType g) {
    switch (g) {
        case GeneratorType::Orbit: return kOrbitParams;
        case GeneratorType::Lissajous: return kLissParams;
        case GeneratorType::Wave: return kWaveParams;
        case GeneratorType::Arc: return kArcParams;
        case GeneratorType::Spiral: return kSpiralParams;
    }
    return {};
}

EnvTrigger EnvTrigger::defaults(int index) {
    //  General MIDI drum map: C1 kick, D1 snare, F#1 closed hi-hat.
    static constexpr int kNotes[] = {36, 38, 42};
    EnvTrigger t;
    t.noteLow = t.noteHigh = kNotes[std::clamp(index, 0, 2)];
    return t;
}

PluginState::PluginState(const ParamManifest& m) {
    resetParamsToDefaults(m);
    for (std::size_t i = 0; i < envTriggers.size(); ++i) envTriggers[i] = EnvTrigger::defaults(static_cast<int>(i));
}

void PluginState::resetParamsToDefaults(const ParamManifest& m) {
    for (const auto& p : m.descs) params[static_cast<std::size_t>(p.id)] = p.def;
}

Json toJson(Product p, const ParamManifest& m, const PluginState& s) {
    Json root = Json::object();
    root.set("version", Json{stateVersionFor(p)});
    root.set("product", Json{name(p)});  // which plugin wrote this

    Json id = Json::object();
    id.set("session", Json{s.identity.session.toString()});
    id.set("instance", Json{s.identity.instance.toString()});
    id.set("label", Json{s.identity.label});
    id.set("colour", Json{s.identity.colour});
    root.set("identity", id);

    //  sparse: a patch that came from no preset says nothing
    if (!s.preset.none()) {
        Json from = Json::object();
        from.set("factory", Json{s.preset.factory});
        from.set("folder", Json{s.preset.folder});
        from.set("name", Json{s.preset.name});
        root.set("preset", from);
    }

    // Sparse: an effect's document carries no trajectory section, since the trajectory is the
    // encoder's alone and an absent one reads back as the default it was.
    if (!isDefaultTrajectory(s.trajectory)) {
        Json traj = Json::object();
        traj.set("kind", Json{name(s.trajectory.kind)});
        traj.set("closed", Json{s.trajectory.closed});
        traj.set("generator", Json{name(s.trajectory.generator)});

        Json gp = Json::object();  // by NAME, so a generator can gain a parameter safely
        const auto names = generatorParamNames(s.trajectory.generator);
        for (std::size_t i = 0; i < names.size() && i < kMaxGenParams; ++i)
            gp.set(names[i], Json{s.trajectory.genParams[i]});
        traj.set("gen_params", gp);

        Json nodes = Json::array();
        for (const auto& n : s.trajectory.nodes) {
            Json jn = Json::object();
            jn.set("p", vecToJson(n.p));
            jn.set("cin", vecToJson(n.cin));
            jn.set("cout", vecToJson(n.cout));
            jn.set("smooth", Json{n.smooth});
            nodes.push(jn);
        }
        traj.set("nodes", nodes);
        root.set("trajectory", traj);
    }

    Json mtx = Json::array();
    for (const auto& c : s.matrix) {
        if (c.target == kNoParamId) continue;
        Json jc = Json::object();
        jc.set("tab", Json{name(c.tab)});
        jc.set("source", Json{c.source});
        jc.set("target", Json{m[static_cast<int>(c.target)].key});  // by key, never by ordinal
        jc.set("depth", Json{c.depth});
        mtx.push(jc);
    }
    root.set("matrix", mtx);

    // Sparse, keyed by index: only sources that differ from the default appear. Eighteen
    // identical {"input":"self"} entries would be seventy lines of noise in a file people
    // hand-edit, and a keyed object also survives kNumSources changing.
    Json srcs = Json::object();
    for (std::size_t i = 0; i < kNumSources; ++i) {
        const auto& sl = s.sources[i];
        if (sl.input == SourceInput::Self && sl.link.isNil()) continue;
        Json js = Json::object();
        js.set("input", Json{name(sl.input)});
        if (sl.input == SourceInput::Link) js.set("link", Json{sl.link.toString()});
        srcs.set(std::to_string(i), js);
    }
    root.set("sources", srcs);

    // Sparse for the same reason as sources: three default entries are noise in a file people
    // read. A trigger that differs from its default is written WHOLE, MIDI and audio halves both,
    // so an input switched back loses nothing. The audio source is written as an INDEX because a
    // modulation slot has no stable key of its own -- if that changes, this is the one place to
    // migrate.
    Json envs = Json::object();
    for (std::size_t i = 0; i < s.envTriggers.size(); ++i) {
        const auto& t = s.envTriggers[i];
        if (t == EnvTrigger::defaults(static_cast<int>(i))) continue;
        Json jt = Json::object();
        jt.set("input", Json{name(t.input)});
        jt.set("note_low", Json{t.noteLow});
        jt.set("note_high", Json{t.noteHigh});
        jt.set("channel", Json{t.channel});
        jt.set("gate", Json{name(t.gate)});
        jt.set("velocity", Json{t.velocity});
        jt.set("source", Json{t.source});
        jt.set("threshold", Json{t.threshold});
        jt.set("hysteresis", Json{t.hysteresis});
        envs.set(std::to_string(i), jt);
    }
    root.set("env_triggers", envs);

    //  Sparse, and keyed by the prefix the region's parameters carry, so a file reads "region1" in
    //  both places. What a region is and how it is used are two sections: see RegionEntry.
    Json regions = Json::object(), uses = Json::object();
    for (int i = 0; i < m.regionCount(); ++i) {
        const auto& r = s.regions[static_cast<std::size_t>(i)];
        const std::string key = "region" + std::to_string(i + 1);
        if (!(r.shape == RegionShape{})) {
            Json jr = Json::object();
            jr.set("kind", Json{name(r.shape.kind)});
            jr.set("sectors", Json{r.shape.sectors});
            jr.set("dots", Json{r.shape.dots});
            jr.set("seed", Json{r.shape.seed});
            // custom's weights define the shape; every other kind has none worth a line
            if (r.shape.custom) {
                jr.set("custom", Json{true});
                if (r.shape.gains != RegionShape{}.gains) {
                    Json gains = Json::array();
                    for (const double v : r.shape.gains) gains.push(Json{v});
                    jr.set("gains", gains);
                }
                Json w = Json::array();
                for (const double v : r.shape.weights) w.push(Json{v});
                jr.set("weights", w);
            }
            regions.set(key, jr);
        }
    }
    root.set("regions", regions);

    // Sparse: a plugin with no quality switch writes nothing, and an absent value reads back
    // as the default it was.
    if (s.renderQuality != PluginState{}.renderQuality)
        root.set("render_quality", Json{std::string(name(s.renderQuality))});

    // A starting point's selection, by name, sparse like the quality above.
    if (!(s.room == RoomState{})) {
        Json jr = Json::object();
        jr.set("preset", Json{std::string(kRoomNames[static_cast<std::size_t>(std::clamp(s.room.preset, 0, 5))])});
        jr.set("shape", Json{std::string(kRoomShapeNames[static_cast<std::size_t>(std::clamp(s.room.shape, 0, 2))])});
        root.set("room", jr);
    }
    if (s.echoPattern != PluginState{}.echoPattern)
        root.set("pattern",
                 Json{std::string(kEchoPatternNames[static_cast<std::size_t>(std::clamp(s.echoPattern, 0, 4))])});
    if (s.windowScale != PluginState{}.windowScale) {
        Json window = Json::object();
        window.set("scale", Json{static_cast<double>(s.windowScale)});
        root.set("window", window);
    }

    Json params = Json::object();
    for (const auto& p : m.descs)
        params.set(p.key, Json{static_cast<double>(s.params[static_cast<std::size_t>(p.id)])});
    root.set("parameters", params);

    return root;
}

LoadResult fromJson(Product p, const ParamManifest& m, const Json& j, PluginState& out) {
    LoadResult r;
    if (!j.isObject()) {
        r.message = "state is not a JSON object";
        return r;
    }

    const Json* ver = j.find("version");
    if (ver == nullptr || !ver->isNumber()) {
        r.message = "state has no version field";
        return r;
    }
    r.fromVersion = static_cast<int>(ver->numberOr(0));
    if (r.fromVersion < 1) {
        r.message = "state version " + std::to_string(r.fromVersion) + " is not valid";
        return r;
    }

    // A document from another plugin, or one that doesn't say, is refused outright and nothing
    // is loaded: the keys both plugins share would land and the writer's own would not.
    r.product = j.find("product") != nullptr ? productFromName(j.find("product")->stringOr("")) : Product::Unknown;
    if (r.product != p) {
        r.fromWrongProduct = true;
        r.message =
            std::string("this is a ") + std::string(name(r.product)) + " patch, not a " + std::string(name(p)) + " one";
        return r;
    }

    if (r.fromVersion > stateVersionFor(p)) {
        r.fromFuture = true;
        r.message = "written by a newer version (" + std::to_string(r.fromVersion) + " > " +
                    std::to_string(stateVersionFor(p)) + "); loaded as far as understood";
    }

    // No migrations: everything is read by name, so a key this build lacks is ignored and one
    // the document lacks takes its default.
    PluginState s{m};  // starts at this manifest's defaults, so anything absent is simply defaulted

    if (const Json* id = j.find("identity"); id != nullptr) {
        if (const Json* v = id->find("session"))
            if (auto u = Uuid::parse(v->stringOr(""))) s.identity.session = *u;
        if (const Json* v = id->find("instance"))
            if (auto u = Uuid::parse(v->stringOr(""))) s.identity.instance = *u;
        if (const Json* v = id->find("label")) s.identity.label = std::string(v->stringOr(""));
        if (const Json* v = id->find("colour"))
            s.identity.colour = std::clamp(static_cast<int>(v->numberOr(0)), 0, kNumColours - 1);
    }

    if (const Json* from = j.find("preset"); from != nullptr && from->isObject()) {
        if (const Json* v = from->find("factory")) s.preset.factory = v->boolOr(false);
        if (const Json* v = from->find("folder")) s.preset.folder = std::string(v->stringOr(""));
        if (const Json* v = from->find("name")) s.preset.name = std::string(v->stringOr(""));
    }

    if (const Json* t = j.find("trajectory"); t != nullptr) {
        if (const Json* v = t->find("kind"))
            s.trajectory.kind = parseEnum(v->stringOr(""), kKindNames, TrajectoryKind::Parametric);
        if (const Json* v = t->find("closed")) s.trajectory.closed = v->boolOr(true);
        if (const Json* v = t->find("generator"))
            s.trajectory.generator = parseEnum(v->stringOr(""), kGenNames, GeneratorType::Orbit);

        if (const Json* gp = t->find("gen_params"); gp != nullptr && gp->isObject()) {
            const auto names = generatorParamNames(s.trajectory.generator);
            for (std::size_t i = 0; i < names.size() && i < kMaxGenParams; ++i)
                if (const Json* v = gp->find(names[i])) s.trajectory.genParams[i] = v->numberOr(0);
        }

        if (const Json* nodes = t->find("nodes"); nodes != nullptr && nodes->isArray()) {
            for (const auto& jn : nodes->items()) {
                Node n;
                n.p = vecFromJson(jn.find("p"), n.p);
                n.cin = vecFromJson(jn.find("cin"), n.cin);
                n.cout = vecFromJson(jn.find("cout"), n.cout);
                if (const Json* v = jn.find("smooth")) n.smooth = v->boolOr(true);
                s.trajectory.nodes.push_back(n);
            }
        }

        // More nodes than this build allows: hand-edited, or written by a build with a higher
        // cap. Truncating would keep the first nodes of a loop and discard the rest of its
        // shape, so the curve is refit to the cap instead, as close to the original as it allows.
        if (s.trajectory.nodes.size() > static_cast<std::size_t>(kMaxNodes)) {
            const std::size_t had = s.trajectory.nodes.size();
            if (s.trajectory.kind == TrajectoryKind::Custom) {
                std::vector<Vec3> dense;
                for (const auto& sample : samplePath(s.trajectory)) dense.push_back(sample.p);
                const FitResult fit = fitNodes(s.trajectory, dense, kMaxNodes, s.trajectory.closed);
                if (!r.message.empty()) r.message += "; ";
                r.message += "trajectory had " + std::to_string(had) + " nodes, refit to " + std::to_string(fit.nodes) +
                             " (limit " + std::to_string(kMaxNodes) + "), max deviation " +
                             std::to_string(fit.maxDeviationRad * kRad2Deg) + " deg";
            } else {
                //  Unused while the trajectory is parametric; the invariant holds anyway.
                s.trajectory.nodes.resize(static_cast<std::size_t>(kMaxNodes));
            }
        }
    }

    if (const Json* mtx = j.find("matrix"); mtx != nullptr && mtx->isArray()) {
        for (const auto& jc : mtx->items()) {
            MatrixCell c;
            // A tab this build does not have: drop the cell, as one aimed at an unknown parameter is.
            bool knownTab = true;
            if (const Json* v = jc.find("tab")) {
                const std::string_view tab = v->stringOr("");
                knownTab = std::find(kTabNames.begin(), kTabNames.end(), tab) != kTabNames.end();
                c.tab = parseEnum(tab, kTabNames, MatrixTab::Features);
            }
            if (!knownTab) continue;
            if (const Json* v = jc.find("source")) c.source = static_cast<int>(v->numberOr(0));
            if (const Json* v = jc.find("target"))
                c.target = [&] {
                    const int at = m.byKey(v->stringOr(""));
                    return at == kNoParam ? kNoParamId : static_cast<ParamId>(at);
                }();
            if (const Json* v = jc.find("depth")) c.depth = v->numberOr(0);
            // A cell aimed at a parameter this build does not have is dropped, not guessed.
            if (c.target != kNoParamId) s.matrix.push_back(c);
        }
    }

    if (const Json* srcs = j.find("sources"); srcs != nullptr && srcs->isObject()) {
        for (const auto& kv : srcs->members()) {
            const long idx = slotNumber(kv.first);
            if (idx < 0 || idx >= kNumSources) continue;  // an index this build lacks: ignore
            auto& slot = s.sources[static_cast<std::size_t>(idx)];
            if (const Json* v = kv.second.find("input"))
                slot.input = parseEnum(v->stringOr(""), kInputNames, SourceInput::Self);
            if (const Json* v = kv.second.find("link"))
                if (auto u = Uuid::parse(v->stringOr(""))) slot.link = *u;
        }
    }

    if (const Json* envs = j.find("env_triggers"); envs != nullptr && envs->isObject()) {
        for (const auto& kv : envs->members()) {
            const long idx = slotNumber(kv.first);
            if (idx < 0 || idx >= static_cast<long>(s.envTriggers.size())) continue;
            auto& t = s.envTriggers[static_cast<std::size_t>(idx)];
            const Json& e = kv.second;
            if (const Json* v = e.find("input"))
                t.input = parseEnum(v->stringOr(""), kTriggerInputNames, TriggerInput::Midi);
            if (const Json* v = e.find("note_low"))
                t.noteLow = std::clamp(static_cast<int>(v->numberOr(t.noteLow)), 0, 127);
            if (const Json* v = e.find("note_high"))
                t.noteHigh = std::clamp(static_cast<int>(v->numberOr(t.noteHigh)), 0, 127);
            if (t.noteLow > t.noteHigh) std::swap(t.noteLow, t.noteHigh);  // a reversed range, put in order
            if (const Json* v = e.find("channel")) t.channel = std::clamp(static_cast<int>(v->numberOr(0)), 0, 16);
            if (const Json* v = e.find("gate"))
                t.gate = parseEnum(v->stringOr(""), kTriggerGateNames, TriggerGate::OneShot);
            if (const Json* v = e.find("velocity")) t.velocity = std::clamp(v->numberOr(0.0), 0.0, 1.0);
            if (const Json* v = e.find("source")) t.source = static_cast<int>(v->numberOr(1));
            if (const Json* v = e.find("threshold")) t.threshold = v->numberOr(0.35);
            if (const Json* v = e.find("hysteresis")) t.hysteresis = v->numberOr(0.10);
        }
    }

    // Bounded by how many slots this plugin has, not by how many the array holds: a document
    // naming `region3` in a plugin with one slot has no parameters for it and cannot be heard.
    const auto regionIndex = [&m](const std::string& key) -> long {
        if (!key.starts_with("region")) return -1;
        const long n = slotNumber(std::string_view(key).substr(6));
        return n >= 1 && n <= m.regionCount() ? n - 1 : -1;
    };
    if (const Json* regions = j.find("regions"); regions != nullptr && regions->isObject()) {
        for (const auto& kv : regions->members()) {
            const long idx = regionIndex(kv.first);
            if (idx < 0) continue;
            RegionShape shape = s.regions[static_cast<std::size_t>(idx)].shape;
            if (const Json* v = kv.second.find("kind"))
                shape.kind = parseEnum(v->stringOr(""), kRegionKindNames, RegionShape{}.kind);
            if (const Json* v = kv.second.find("sectors")) shape.sectors = static_cast<int>(v->numberOr(shape.sectors));
            if (const Json* v = kv.second.find("dots")) shape.dots = static_cast<int>(v->numberOr(shape.dots));
            if (const Json* v = kv.second.find("seed")) shape.seed = static_cast<int>(v->numberOr(shape.seed));
            if (const Json* v = kv.second.find("custom")) shape.custom = v->boolOr(false);
            if (const Json* v = kv.second.find("gains"); v != nullptr && v->isArray()) {
                const auto& items = v->items();
                for (std::size_t i = 0; i < items.size() && i < shape.gains.size(); ++i)
                    shape.gains[i] = items[i].numberOr(1.0);
            }
            if (const Json* v = kv.second.find("weights"); v != nullptr && v->isArray()) {
                shape.weights = {};
                const auto& items = v->items();
                for (std::size_t i = 0; i < items.size() && i < shape.weights.size(); ++i)
                    shape.weights[i] = items[i].numberOr(0.0);
            }
            s.regions[static_cast<std::size_t>(idx)].shape = sanitised(shape);
        }
    }
    if (const Json* q = j.find("render_quality"); q != nullptr)
        s.renderQuality = parseEnum(q->stringOr(""), kRenderQualityNames, PluginState{}.renderQuality);
    if (const Json* room = j.find("room"); room != nullptr && room->isObject()) {
        if (const Json* v = room->find("preset"))
            s.room.preset = parseEnum(v->stringOr(""), kRoomNames, RoomState{}.preset);
        if (const Json* v = room->find("shape"))
            s.room.shape = parseEnum(v->stringOr(""), kRoomShapeNames, RoomState{}.shape);
    }
    if (const Json* p = j.find("pattern"); p != nullptr)
        s.echoPattern = parseEnum(p->stringOr(""), kEchoPatternNames, PluginState{}.echoPattern);
    if (const Json* window = j.find("window"); window != nullptr && window->isObject())
        if (const Json* v = window->find("scale"))
            s.windowScale = std::clamp(static_cast<float>(v->numberOr(1.0)), kWindowScaleMin, kWindowScaleMax);

    if (const Json* params = j.find("parameters"); params != nullptr && params->isObject()) {
        for (const auto& kv : params->members()) {
            const int at = m.byKey(kv.first);
            const ParamId id = at == kNoParam ? kNoParamId : static_cast<ParamId>(at);
            if (id == kNoParamId) continue;  // a key this build does not know: ignore
            s.params[static_cast<std::size_t>(id)] =
                clampToRange(m, at, static_cast<float>(kv.second.numberOr(m[at].def)));
        }
    }

    out = std::move(s);
    r.ok = true;
    return r;
}

int stateVersionFor(Product p) { return p == Product::Encoder ? kEncoderStateVersion : kEffectStateVersion; }

std::string saveState(Product p, const ParamManifest& m, const PluginState& s) { return toJson(p, m, s).dump(2); }

LoadResult loadState(Product p, const ParamManifest& m, std::string_view text, PluginState& out) {
    std::string err;
    const Json j = Json::parse(text, &err);
    if (!err.empty()) {
        LoadResult r;
        r.message = "malformed state: " + err;
        return r;
    }
    return fromJson(p, m, j, out);
}

}  // namespace bambi
