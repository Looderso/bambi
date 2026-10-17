// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/remote.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "bambi/decode/layouts.hpp"

namespace bambi {

namespace {

void add(std::string& notice, const std::string& more) {
    if (more.empty()) return;
    notice += notice.empty() ? more : "; " + more;
}

std::optional<int> channelOf(const Json& j, const char* key) {
    const Json* v = j.find(key);
    if (v == nullptr || !v->isNumber()) return std::nullopt;
    return static_cast<int>(std::lround(v->numberOr(0.0)));
}

std::optional<bool> boolOf(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v != nullptr && v->isBool() ? std::optional<bool>(v->boolOr(false)) : std::nullopt;
}

Json channels(const std::vector<int>& list) {
    Json a = Json::array();
    for (const int c : list) a.push(Json(c));
    return a;
}

}  // namespace

DecoderReport reportOf(const DecoderState& d, int bus, double levelDb, std::string notice) {
    DecoderReport r;
    r.bus = bus;
    r.headphonesOn = d.headphonesPlay;
    r.headphoneChannel = d.headphoneChannel;
    r.speakersOn = d.speakersPlay;
    r.speakerFirstChannel = std::max(1, d.speakerFirstChannel);
    const bool builtIn =
        d.matrixFile.empty() && !d.layoutTyped && !d.layout.empty() && bs2051Layout(d.layoutName) != nullptr;
    r.layout = builtIn ? d.layoutName : std::string();
    r.layoutName = !d.matrixFile.empty() ? d.matrixFile : d.layout.empty() ? std::string() : d.layoutName;
    r.speakerChannels = outputChannels(d, Output::Speakers);
    for (const std::string_view n : bs2051Names()) r.layouts.emplace_back(n);
    r.levelDb = levelDb;
    r.notice = std::move(notice);
    return r;
}

Json toJson(const DecoderReport& r) {
    Json j = Json::object();
    j.set("bus", Json(r.bus));
    j.set("headphonesOn", Json(r.headphonesOn));
    j.set("headphoneChannel", Json(r.headphoneChannel));
    j.set("speakersOn", Json(r.speakersOn));
    j.set("speakerFirstChannel", Json(r.speakerFirstChannel));
    j.set("layout", Json(r.layout));
    j.set("layoutName", Json(r.layoutName));
    j.set("speakerChannels", channels(r.speakerChannels));
    Json names = Json::array();
    for (const std::string& n : r.layouts) names.push(Json(n));
    j.set("layouts", std::move(names));
    j.set("levelDb", Json(r.levelDb));
    j.set("notice", Json(r.notice));
    return j;
}

DecoderReport reportFrom(const Json& j) {
    DecoderReport r;
    r.bus = channelOf(j, "bus").value_or(0);
    r.headphonesOn = boolOf(j, "headphonesOn").value_or(false);
    r.headphoneChannel = channelOf(j, "headphoneChannel").value_or(1);
    r.speakersOn = boolOf(j, "speakersOn").value_or(false);
    r.speakerFirstChannel = channelOf(j, "speakerFirstChannel").value_or(1);
    if (const Json* v = j.find("layout")) r.layout = std::string(v->stringOr(""));
    if (const Json* v = j.find("layoutName")) r.layoutName = std::string(v->stringOr(""));
    if (const Json* list = j.find("speakerChannels"); list != nullptr && list->isArray())
        for (const Json& c : list->items())
            if (c.isNumber()) r.speakerChannels.push_back(static_cast<int>(std::lround(c.numberOr(0.0))));
    if (const Json* list = j.find("layouts"); list != nullptr && list->isArray())
        for (const Json& n : list->items())
            if (n.isString()) r.layouts.emplace_back(n.stringOr(""));
    if (const Json* v = j.find("levelDb")) r.levelDb = v->numberOr(0.0);
    if (const Json* v = j.find("notice")) r.notice = std::string(v->stringOr(""));
    return r;
}

Json toJson(const DecoderRequest& r) {
    Json j = Json::object();
    if (r.headphonesOn) j.set("headphonesOn", Json(*r.headphonesOn));
    if (r.headphoneChannel) j.set("headphoneChannel", Json(*r.headphoneChannel));
    if (r.speakersOn) j.set("speakersOn", Json(*r.speakersOn));
    if (r.speakerFirstChannel) j.set("speakerFirstChannel", Json(*r.speakerFirstChannel));
    if (r.layout) j.set("layout", Json(*r.layout));
    if (r.levelDb) j.set("levelDb", Json(*r.levelDb));
    return j;
}

DecoderRequest requestFrom(const Json& j) {
    DecoderRequest r;
    r.headphonesOn = boolOf(j, "headphonesOn");
    r.headphoneChannel = channelOf(j, "headphoneChannel");
    r.speakersOn = boolOf(j, "speakersOn");
    r.speakerFirstChannel = channelOf(j, "speakerFirstChannel");
    if (const Json* v = j.find("layout"); v != nullptr && v->isString()) r.layout = std::string(v->stringOr(""));
    if (const Json* v = j.find("levelDb"); v != nullptr && v->isNumber()) r.levelDb = v->numberOr(0.0);
    return r;
}

Lit grant(DecoderState d, const DecoderRequest& r, int bus) {
    std::string notice;
    if (r.layout) {
        if (const Layout* l = bs2051Layout(*r.layout)) {
            d.layout = l->speakers;
            d.layoutName = *r.layout;
            d.layoutTyped = false;
            d.matrixFile.clear();
            d.matrixOutputs = 0;
        } else {
            add(notice, "no built-in layout \"" + *r.layout + "\"");
        }
    }
    if (const auto c = r.headphoneChannel) {
        if (*c >= 1 && *c % 2 == 1 && *c + 1 <= bus)
            d.headphoneChannel = *c;
        else
            add(notice, "headphones cannot start on channel " + std::to_string(*c));
    }
    if (const auto c = r.speakerFirstChannel) {
        DecoderState moved = d;
        moved.speakerFirstChannel = *c;
        const std::vector<int> mine = outputChannels(moved, Output::Speakers);
        if (*c >= 1 && *c <= bus && (mine.empty() || mine.back() <= bus))
            d = std::move(moved);
        else
            add(notice, "speakers cannot start on channel " + std::to_string(*c));
    }
    for (const auto& [which, on] : {std::pair{Output::Headphones, r.headphonesOn}, {Output::Speakers, r.speakersOn}})
        if (on) {
            Lit lit = lightOutput(std::move(d), which, *on, bus);
            d = std::move(lit.state);
            add(notice, lit.notice);
        }
    return {std::move(d), notice};
}

}  // namespace bambi
