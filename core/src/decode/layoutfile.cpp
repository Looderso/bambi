// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/layoutfile.hpp"

#include "bambi/patch/json.hpp"

namespace bambi {

bool parseIemLayout(std::string_view json, Layout& out, std::string* error) {
    const Json root = Json::parse(json, error);
    if (!root.isObject()) {
        if (error && error->empty()) *error = "not a JSON object";
        return false;
    }
    const Json* layout = root.find("LoudspeakerLayout");
    const Json* list = layout ? layout->find("Loudspeakers") : nullptr;
    if (list == nullptr || !list->isArray()) {
        if (error) *error = "no LoudspeakerLayout.Loudspeakers list";
        return false;
    }
    out.speakers.clear();
    for (const Json& item : list->items()) {
        const Json* az = item.find("Azimuth");
        const Json* el = item.find("Elevation");
        const Json* ch = item.find("Channel");
        if (!az || !az->isNumber() || !el || !el->isNumber() || !ch || !ch->isNumber()) {
            if (error) *error = "a loudspeaker without Azimuth, Elevation and Channel";
            return false;
        }
        Loudspeaker s;
        s.azimuthDeg = az->numberOr(0);
        s.elevationDeg = el->numberOr(0);
        s.channel = static_cast<int>(ch->numberOr(0));
        if (const Json* r = item.find("Radius")) s.radiusM = r->numberOr(1.0);
        if (const Json* g = item.find("Gain")) s.gain = g->numberOr(1.0);
        if (const Json* im = item.find("IsImaginary")) s.imaginary = im->boolOr(false);
        out.speakers.push_back(s);
    }
    if (out.speakers.empty()) {
        if (error) *error = "no loudspeakers";
        return false;
    }
    return true;
}

}  // namespace bambi
