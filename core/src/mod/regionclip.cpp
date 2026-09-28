// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/mod/regionclip.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/mod/matrix.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/json.hpp"

namespace bambi {
namespace {

constexpr int kClipVersion = 1;
constexpr std::string_view kMarker = "bambi.region";
constexpr std::array<std::string_view, 7> kKinds{"everywhere", "spot", "band", "sectors", "dots", "clouds", "custom"};

/*  What a region's definition is made of: the region block's fields (`patch/blocks.hpp`) less the
 *  side, which is the use's. A descriptor holds exactly these -- one missing is an older region, one
 *  extra a newer, and either is a guess. */
constexpr std::array<std::string_view, 16> kFields{
    "yaw",       "pitch", "roll",     "yaw_rate", "pitch_rate", "roll_rate", "softness", "size", "band_elevation",
    "thickness", "fill",  "dot_size", "coverage", "contrast",   "detail",    "evolve"};
constexpr std::array<std::string_view, 8> kLfoFields{"index", "rate",  "sync",     "div",
                                                     "shape", "phase", "polarity", "amount"};

/// The field of a key in this region's block, or empty: "region1.yaw" is "yaw" for "region1".
std::string_view fieldOf(std::string_view key, std::string_view prefix) {
    if (key.size() <= prefix.size() + 1 || key.substr(0, prefix.size()) != prefix || key[prefix.size()] != '.')
        return {};
    return key.substr(prefix.size() + 1);
}

/// The side is the use's, not the region's: it is on no clip, in either direction.
bool travels(std::string_view field) { return !field.empty() && field != "side"; }

float valueAt(const PluginState& s, int at) { return at == kNoParam ? 0.0f : s.params[static_cast<std::size_t>(at)]; }

}  // namespace

RegionClip copyRegion(const ModManifest& mods, const PluginState& s, int slot, std::string_view prefix) {
    RegionClip clip;
    const ParamManifest& m = *mods.params;
    clip.shape = s.regions[static_cast<std::size_t>(slot)].shape;
    for (int at = 0; at < m.size(); ++at)
        if (const auto field = fieldOf(m[at].key, prefix); travels(field))
            clip.fields.emplace_back(std::string(field), valueAt(s, at));

    for (const MatrixCell& cell : s.matrix) {
        if (cell.tab != MatrixTab::Generators || cell.source < 0 || cell.source >= kNumLfos || cell.depth == 0.0)
            continue;
        const int target = static_cast<int>(cell.target);
        if (!m.has(target) || !travels(fieldOf(m[target].key, prefix))) continue;
        const LfoParams& lfo = mods.lfos[static_cast<std::size_t>(cell.source)];
        if (valueAt(s, lfo.retrigger) > 0.5f)
            continue;  // set to continue: it agrees with nothing, so it is left behind

        clip.rows.push_back({cell.source, std::string(fieldOf(m[target].key, prefix)), cell.depth});
        const bool have = std::any_of(clip.lfos.begin(), clip.lfos.end(),
                                      [&](const RegionClip::Lfo& l) { return l.index == cell.source; });
        if (have) continue;
        RegionClip::Lfo l;
        l.index = cell.source;
        l.rate = valueAt(s, lfo.rate);
        l.sync = valueAt(s, lfo.sync);
        l.div = valueAt(s, lfo.div);
        l.shape = valueAt(s, lfo.shape);
        l.phase = valueAt(s, lfo.phase);
        l.polarity = valueAt(s, lfo.polarity);
        l.amount = valueAt(s, mods.amountOf(sourceSlot(MatrixTab::Generators, cell.source)));
        clip.lfos.push_back(l);
    }
    return clip;
}

void pasteRegion(const ModManifest& mods, const RegionClip& clip, int slot, std::string_view prefix, PluginState& state,
                 std::vector<std::pair<int, float>>& writes) {
    const ParamManifest& m = *mods.params;
    const auto write = [&](int at, float value) {
        if (at != kNoParam) writes.emplace_back(at, clampToRange(m, at, value));
    };
    const auto keyOf = [&](const std::string& field) { return m.byKey(std::string(prefix) + "." + field); };

    state.regions[static_cast<std::size_t>(slot)].shape = sanitised(clip.shape);
    for (const auto& [field, value] : clip.fields)
        if (travels(field)) write(keyOf(field), value);

    //  every LFO row onto this region goes; the clip's come back
    state.matrix.erase(std::remove_if(state.matrix.begin(), state.matrix.end(),
                                      [&](const MatrixCell& cell) {
                                          const int target = static_cast<int>(cell.target);
                                          return cell.tab == MatrixTab::Generators && cell.source < kNumLfos &&
                                                 m.has(target) && travels(fieldOf(m[target].key, prefix));
                                      }),
                       state.matrix.end());
    for (const RegionClip::Row& row : clip.rows) {
        const int target = keyOf(row.field);
        if (row.lfo < 0 || row.lfo >= kNumLfos || target == kNoParam || !travels(row.field)) continue;
        setCellDepth(mods, state, MatrixTab::Generators, row.lfo, static_cast<ParamId>(target), row.depth);
    }
    for (const RegionClip::Lfo& l : clip.lfos) {
        if (l.index < 0 || l.index >= kNumLfos) continue;
        const LfoParams& lfo = mods.lfos[static_cast<std::size_t>(l.index)];
        write(lfo.rate, l.rate);
        write(lfo.sync, l.sync);
        write(lfo.div, l.div);
        write(lfo.shape, l.shape);
        write(lfo.phase, l.phase);
        write(lfo.polarity, l.polarity);
        write(lfo.retrigger, 0.0f);  // restart: the only kind a clip carries
        write(mods.amountOf(sourceSlot(MatrixTab::Generators, l.index)), l.amount);
    }
}

std::string writeRegionClip(const RegionClip& clip) {
    Json j = Json::object();
    j.set(kMarker, Json(static_cast<double>(kClipVersion)));
    j.set("kind",
          Json(std::string(kKinds[static_cast<std::size_t>(std::clamp(static_cast<int>(clip.shape.kind), 0, 4))])));
    j.set("sectors", Json(static_cast<double>(clip.shape.sectors)));
    j.set("dots", Json(static_cast<double>(clip.shape.dots)));

    Json fields = Json::object();
    for (const auto& [field, value] : clip.fields) fields.set(field, Json(static_cast<double>(value)));
    j.set("fields", std::move(fields));

    Json lfos = Json::array();
    for (const RegionClip::Lfo& l : clip.lfos) {
        Json o = Json::object();
        o.set("index", Json(static_cast<double>(l.index)));
        o.set("rate", Json(static_cast<double>(l.rate)));
        o.set("sync", Json(static_cast<double>(l.sync)));
        o.set("div", Json(static_cast<double>(l.div)));
        o.set("shape", Json(static_cast<double>(l.shape)));
        o.set("phase", Json(static_cast<double>(l.phase)));
        o.set("polarity", Json(static_cast<double>(l.polarity)));
        o.set("amount", Json(static_cast<double>(l.amount)));
        lfos.push(std::move(o));
    }
    j.set("lfos", std::move(lfos));

    Json rows = Json::array();
    for (const RegionClip::Row& r : clip.rows) {
        Json o = Json::object();
        o.set("lfo", Json(static_cast<double>(r.lfo)));
        o.set("field", Json(r.field));
        o.set("depth", Json(r.depth));
        rows.push(std::move(o));
    }
    j.set("rows", std::move(rows));
    return j.dump(0);
}

std::optional<RegionClip> readRegionClip(std::string_view text) {
    //  a clipboard holds anything, of any size: nothing is parsed that does not say what it is
    if (text.size() > 16384 || text.find(kMarker) == std::string_view::npos) return std::nullopt;
    std::string error;
    const Json j = Json::parse(text, &error);
    if (!error.empty() || !j.isObject()) return std::nullopt;
    const Json* version = j.find(kMarker);
    if (version == nullptr || !version->isNumber() || version->numberOr(0.0) != static_cast<double>(kClipVersion))
        return std::nullopt;  // 1.9 is not 1

    const Json* kind = j.find("kind");
    const Json* fields = j.find("fields");
    if (kind == nullptr || !kind->isString() || fields == nullptr || !fields->isObject()) return std::nullopt;
    const auto named = std::find(kKinds.begin(), kKinds.end(), kind->stringOr({}));
    if (named == kKinds.end()) return std::nullopt;

    RegionClip clip;
    clip.shape.kind = static_cast<RegionKind>(named - kKinds.begin());
    if (const Json* n = j.find("sectors")) clip.shape.sectors = static_cast<int>(n->numberOr(clip.shape.sectors));
    if (const Json* n = j.find("dots")) clip.shape.dots = static_cast<int>(n->numberOr(clip.shape.dots));
    clip.shape = sanitised(clip.shape);

    //  exactly a region's fields, each a finite number. A `side` written in by hand is not one of them.
    if (fields->members().size() != kFields.size()) return std::nullopt;
    for (const std::string_view field : kFields) {
        const Json* value = fields->find(field);
        if (value == nullptr || !value->isNumber() || !std::isfinite(value->numberOr(0.0))) return std::nullopt;
        clip.fields.emplace_back(std::string(field), static_cast<float>(value->numberOr(0.0)));
    }

    //  a number that is there and finite, or nothing: no fallback stands in for a missing one
    const auto number = [](const Json& o, std::string_view key) -> std::optional<float> {
        const Json* n = o.find(key);
        if (n == nullptr || !n->isNumber() || !std::isfinite(n->numberOr(0.0))) return std::nullopt;
        return static_cast<float>(n->numberOr(0.0));
    };
    if (const Json* lfos = j.find("lfos"); lfos != nullptr) {
        if (!lfos->isArray()) return std::nullopt;
        for (const Json& o : lfos->items()) {
            if (!o.isObject() || o.members().size() != kLfoFields.size()) return std::nullopt;
            std::array<float, kLfoFields.size()> v{};
            for (std::size_t i = 0; i < kLfoFields.size(); ++i) {
                const auto n = number(o, kLfoFields[i]);
                if (!n.has_value()) return std::nullopt;
                v[i] = *n;
            }
            RegionClip::Lfo l;
            l.index = static_cast<int>(v[0]);
            if (static_cast<float>(l.index) != v[0] || l.index < 0 || l.index >= kNumLfos) return std::nullopt;
            l.rate = v[1], l.sync = v[2], l.div = v[3], l.shape = v[4], l.phase = v[5], l.polarity = v[6],
            l.amount = v[7];
            clip.lfos.push_back(l);
        }
    }
    if (const Json* rows = j.find("rows"); rows != nullptr) {
        if (!rows->isArray()) return std::nullopt;
        for (const Json& o : rows->items()) {
            if (!o.isObject()) return std::nullopt;
            const auto lfo = number(o, "lfo");
            const auto depth = number(o, "depth");
            const Json* field = o.find("field");
            if (!lfo.has_value() || !depth.has_value() || field == nullptr || !field->isString()) return std::nullopt;
            RegionClip::Row r;
            r.lfo = static_cast<int>(*lfo);
            r.field = std::string(field->stringOr({}));
            r.depth = static_cast<double>(*depth);
            //  a row is over a field a region has, from an LFO that came with it: its settings are
            //  what make the two agree, and a row without them is half a row
            const bool known = std::find(kFields.begin(), kFields.end(), std::string_view(r.field)) != kFields.end();
            const bool carried = std::any_of(clip.lfos.begin(), clip.lfos.end(),
                                             [&](const RegionClip::Lfo& l) { return l.index == r.lfo; });
            if (!known || !carried) return std::nullopt;
            clip.rows.push_back(std::move(r));
        }
    }
    return clip;
}

std::string describeRegionClip(const RegionClip& clip) {
    std::string out(kKinds[static_cast<std::size_t>(std::clamp(static_cast<int>(clip.shape.kind), 0, 4))]);
    if (clip.shape.kind == RegionKind::Everywhere) return out;
    for (const auto& [field, value] : clip.fields)
        if (field == "softness")
            out += ", " +
                   std::to_string(static_cast<int>(std::lround(std::clamp(value, 0.0f, 180.0f) / 180.0f * 100.0f))) +
                   " % soft";
    // A paste retunes the LFOs it brings -- settings, amount, and restart -- and they may be driving
    // other things here, so it says which.
    if (!clip.lfos.empty()) {
        out += ", sets lfo";
        for (std::size_t i = 0; i < clip.lfos.size(); ++i)
            out += (i == 0 ? " " : " + ") + std::to_string(clip.lfos[i].index + 1);
    }
    return out;
}

}  // namespace bambi
