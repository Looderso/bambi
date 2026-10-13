// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/layoutfile.hpp"

#include <cstdlib>
#include <string>
#include <vector>

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

namespace {
std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

/// The whitespace-separated words of a line, a `#` ending it.
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

bool number(std::string_view word, double& out) {
    const std::string s(word);
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && !s.empty();
}

/// system:playback_7 -> 7; nothing without digits at the end.
bool trailingNumber(std::string_view word, int& out) {
    std::size_t i = word.size();
    while (i > 0 && word[i - 1] >= '0' && word[i - 1] <= '9') --i;
    if (i == word.size()) return false;
    out = 0;
    for (std::size_t k = i; k < word.size(); ++k) out = out * 10 + (word[k] - '0');
    return out >= 1;
}
}  // namespace

bool parseAmbdecLayout(std::string_view text, Layout& out, std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    out.speakers.clear();
    int declared = -1;
    bool inSpeakers = false, sawBlock = false;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line =
            trimmed(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        const std::vector<std::string_view> w = words(line);
        if (w.empty()) continue;
        if (w[0] == "/dec/speakers" && w.size() >= 2) {
            double n = 0;
            if (!number(w[1], n)) return fail("/dec/speakers is not a count");
            declared = static_cast<int>(n);
        } else if (w[0] == "/speakers/{") {
            inSpeakers = true;
            sawBlock = true;
        } else if (w[0] == "/}") {
            inSpeakers = false;
        } else if (inSpeakers) {
            if (w[0] != "add_spkr" || w.size() < 5)
                return fail("a speakers line that is not add_spkr id dist azim elev");
            Loudspeaker s;
            double az = 0, el = 0;
            if (!number(w[2], s.radiusM) || !number(w[3], az) || !number(w[4], el))
                return fail("add_spkr " + std::string(w[1]) + ": distance, azimuth and elevation must be numbers");
            s.azimuthDeg = az;
            s.elevationDeg = el;
            s.channel = static_cast<int>(out.speakers.size()) + 1;
            if (w.size() >= 6) {
                int ch = 0;
                if (trailingNumber(w[5], ch)) s.channel = ch;
            }
            out.speakers.push_back(s);
        }
    }
    if (!sawBlock) return fail("no /speakers/{ block: not an AmbDec configuration");
    if (out.speakers.empty()) return fail("no loudspeakers");
    if (declared >= 0 && declared != static_cast<int>(out.speakers.size()))
        return fail("/dec/speakers says " + std::to_string(declared) + ", the block has " +
                    std::to_string(out.speakers.size()));
    return true;
}

bool parseLayoutFile(std::string_view text, Layout& out, std::string* error) {
    const std::string_view lead = trimmed(text);
    if (!lead.empty() && lead.front() == '{') return parseIemLayout(text, out, error);
    return parseAmbdecLayout(text, out, error);
}

}  // namespace bambi
