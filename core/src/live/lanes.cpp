// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/lanes.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace bambi::live {

namespace {

const char* nameOf(Instance i) {
    return i == Instance::Encoder ? "encoder" : i == Instance::Insert ? "insert" : "decoder";
}

std::optional<Instance> instanceNamed(std::string_view name) {
    if (name == "encoder") return Instance::Encoder;
    if (name == "insert") return Instance::Insert;
    if (name == "decoder") return Instance::Decoder;
    return std::nullopt;
}

}  // namespace

std::optional<double> valueAt(const std::vector<LanePoint>& points, double ppq) {
    if (points.empty()) return std::nullopt;
    if (ppq <= points.front().ppq) return points.front().value;
    if (ppq >= points.back().ppq) return points.back().value;
    const auto after =
        std::upper_bound(points.begin(), points.end(), ppq, [](double t, const LanePoint& p) { return t < p.ppq; });
    const LanePoint& b = *after;
    const LanePoint& a = *(after - 1);
    const double t = (ppq - a.ppq) / (b.ppq - a.ppq);
    return a.value + t * (b.value - a.value);
}

std::size_t putPoint(Lane& lane, LanePoint p) {
    auto& pts = lane.points;
    const auto at =
        std::lower_bound(pts.begin(), pts.end(), p.ppq, [](const LanePoint& x, double t) { return x.ppq < t; });
    if (at != pts.end() && at->ppq == p.ppq) {
        at->value = p.value;
        return static_cast<std::size_t>(at - pts.begin());
    }
    //  the insert first, on its own: it can reallocate, and the order in which one expression's operands are evaluated is not
    //  fixed, so a begin() taken beside it may be of the storage it left
    const auto put = pts.insert(at, p);
    return static_cast<std::size_t>(put - pts.begin());
}

std::size_t movePoint(Lane& lane, std::size_t index, LanePoint to) {
    auto& pts = lane.points;
    if (index >= pts.size()) return index;
    //  held between its neighbours, never onto them: the order and every time stay distinct
    constexpr double kApart = 1.0 / 960.0;
    const double low = index > 0 ? pts[index - 1].ppq + kApart : 0.0;
    const double high = index + 1 < pts.size() ? pts[index + 1].ppq - kApart : to.ppq;
    pts[index] = {std::clamp(to.ppq, low, std::max(low, high)), to.value};
    return index;
}

void removePoint(Lane& lane, std::size_t index) {
    if (index < lane.points.size()) lane.points.erase(lane.points.begin() + static_cast<std::ptrdiff_t>(index));
}

std::optional<std::size_t> pointNear(const Lane& lane, double ppq, double value, double reach, double valueReach) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < lane.points.size(); ++i) {
        const LanePoint& p = lane.points[i];
        if (std::abs(p.ppq - ppq) > reach || std::abs(p.value - value) > valueReach) continue;
        if (!best || std::abs(p.ppq - ppq) < std::abs(lane.points[*best].ppq - ppq)) best = i;
    }
    return best;
}

double clipPosition(const SlotPlay& at, double elapsed) {
    const double p = at.position + std::max(0.0, elapsed);
    const double length = at.loopEnd - at.loopStart;
    if (length <= 0.0) return p;
    if (!at.looping) return std::min(p, at.loopEnd);
    if (p < at.loopEnd) return p;
    return at.loopStart + std::fmod(p - at.loopStart, length);
}

bool followClips(std::vector<Lane>& lanes, const Arrangement& a) {
    bool changed = false;
    for (Lane& l : lanes) {
        if (!l.clip()) continue;
        const auto track =
            std::find_if(a.session.begin(), a.session.end(), [&](const TrackSlots& t) { return t.id == l.clipTrack; });
        if (track == a.session.end()) continue;
        const SessionClip* here = nullptr;
        std::vector<const SessionClip*> named;
        for (const SessionClip& c : track->clips) {
            if (c.slot == l.slot) here = &c;
            if (!l.clipName.empty() && c.name == l.clipName) named.push_back(&c);
        }
        if (here != nullptr && (l.clipName.empty() || here->name == l.clipName)) {
            if (l.clipName.empty() && !here->name.empty()) {  // named only now
                l.clipName = here->name;
                changed = true;
            }
            continue;
        }
        if (named.size() == 1) {  // moved with its scene
            l.slot = named.front()->slot;
            changed = true;
        } else if (named.empty() && here != nullptr) {  // renamed where it stands
            l.clipName = here->name;
            changed = true;
        }
    }
    return changed;
}

Json toJson(const std::vector<Lane>& lanes) {
    Json out = Json::array();
    for (const Lane& l : lanes) {
        Json o = Json::object();
        o.set("id", Json(static_cast<double>(l.id)));
        o.set("instance", Json(nameOf(l.instance)));
        o.set("owner", Json(static_cast<double>(l.owner)));
        o.set("param", Json(l.param));
        Json points = Json::array();
        for (const LanePoint& p : l.points) {
            Json pair = Json::array();
            pair.push(Json(p.ppq));
            pair.push(Json(p.value));
            points.push(std::move(pair));
        }
        o.set("points", std::move(points));
        if (l.clip()) {
            o.set("clipTrack", Json(static_cast<double>(l.clipTrack)));
            o.set("slot", Json(l.slot));
            o.set("clipName", Json(l.clipName));
        }
        out.push(std::move(o));
    }
    return out;
}

std::vector<Lane> lanesFrom(const Json& json) {
    std::vector<Lane> out;
    if (!json.isArray()) return out;
    for (const Json& o : json.items()) {
        const Json* id = o.find("id");
        const Json* instance = o.find("instance");
        const Json* owner = o.find("owner");
        const Json* param = o.find("param");
        const auto kind = instance != nullptr ? instanceNamed(instance->stringOr("")) : std::nullopt;
        if (id == nullptr || id->numberOr(0.0) < 1.0 || !kind || owner == nullptr || param == nullptr ||
            param->stringOr("").empty())
            continue;
        Lane l{static_cast<std::uint64_t>(std::llround(id->numberOr(0.0))),
               *kind,
               static_cast<std::uint64_t>(std::llround(owner->numberOr(0.0))),
               std::string(param->stringOr("")),
               {},
               0,
               -1,
               {}};
        if (const Json* slot = o.find("slot"); slot != nullptr && slot->numberOr(-1.0) >= 0.0) {
            const Json* track = o.find("clipTrack");
            l.clipTrack = track != nullptr ? static_cast<TrackId>(std::llround(track->numberOr(0.0))) : 0;
            l.slot = l.clipTrack != 0 ? static_cast<int>(std::llround(slot->numberOr(-1.0))) : -1;
            if (const Json* name = o.find("clipName")) l.clipName = std::string(name->stringOr(""));
        }
        if (const Json* points = o.find("points"); points != nullptr && points->isArray())
            for (const Json& p : points->items())
                if (p.isArray() && p.size() == 2 && p.items()[0].isNumber() && p.items()[1].isNumber())
                    putPoint(l, {p.items()[0].numberOr(0.0), p.items()[1].numberOr(0.0)});
        out.push_back(std::move(l));
    }
    return out;
}

}  // namespace bambi::live
