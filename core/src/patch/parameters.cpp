// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/parameters.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace bambi {
int ParamManifest::byKey(std::string_view key) const {
    for (int i = 0; i < size(); ++i)
        if (descs[static_cast<std::size_t>(i)].key == key) return i;
    return kNoParam;
}

int ParamManifest::regionCount() const {
    int n = 0;
    while (n < kMaxRegions && byKey("region" + std::to_string(n + 1) + ".side") != kNoParam) ++n;
    return n;
}

float ParamManifest::skewCentreOf(int at) const {
    return has(at) && at < static_cast<int>(skew.size()) ? skew[static_cast<std::size_t>(at)] : 0.0f;
}

DestKind ParamManifest::destKindOf(int at) const {
    return has(at) && at < static_cast<int>(dest.size()) ? dest[static_cast<std::size_t>(at)]
                                                         : DestKind::NotModulatable;
}

double ParamManifest::smoothingOf(int at) const {
    return has(at) && at < static_cast<int>(smoothing.size()) ? smoothing[static_cast<std::size_t>(at)] : 0.0;
}

bool ParamManifest::wrapsAt(int at) const {
    return has(at) && at < static_cast<int>(wraps.size()) && wraps[static_cast<std::size_t>(at)] != 0;
}

float clampToRange(const ParamManifest& m, int at, float value) {
    const auto& p = m[at];
    if (std::isnan(value)) return p.def;
    float v = value < p.min ? p.min : (value > p.max ? p.max : value);
    if (p.type != ParamType::Float) v = std::round(v);
    return v;
}

/// The exponent for a skew centre, matching JUCE's NormalisableRange::setSkewForCentre.
static double skewFor(const ParamDesc& d, float centre) {
    return std::log(0.5) / std::log((static_cast<double>(centre) - d.min) / (static_cast<double>(d.max) - d.min));
}

float toNormalised(const ParamManifest& m, int at, float value) {
    const auto& d = m[at];
    double t = (static_cast<double>(value) - d.min) / (static_cast<double>(d.max) - d.min);
    if (const float centre = m.skewCentreOf(at); centre > 0.0f) {
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        if (t > 0.0) t = std::exp(std::log(t) * skewFor(d, centre));
    }
    return static_cast<float>(t);
}

float fromNormalised(const ParamManifest& m, int at, float normalised) {
    const auto& d = m[at];
    double t = normalised;
    if (const float centre = m.skewCentreOf(at); centre > 0.0f) {
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        if (t > 0.0) t = std::exp(std::log(t) / skewFor(d, centre));
    }
    return static_cast<float>(d.min + t * (static_cast<double>(d.max) - d.min));
}

int choiceCount(const ParamDesc& p) {
    if (p.type != ParamType::Choice || p.choices.empty()) return 0;
    int n = 1;
    for (char c : p.choices)
        if (c == ',') ++n;
    return n;
}

std::string formatNumber(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::max(0, decimals), std::abs(value));
    const bool negative = value < 0.0 && std::strtod(buf, nullptr) > 0.0;
    return negative ? std::string("\xe2\x88\x92") + buf : std::string(buf);
}

std::string formatParameter(const ParamDesc& d, double value) {
    if (d.type == ParamType::Choice) {
        const std::string_view all = d.choices;
        const int index = std::max(0, static_cast<int>(std::lround(value)));
        std::size_t start = 0;
        for (int i = 0;; ++i) {
            const std::size_t comma = all.find(',', start);
            if (i == index || comma == std::string_view::npos)
                return std::string(
                    all.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
            start = comma + 1;
        }
    }
    if (d.type == ParamType::Bool) return value > 0.5 ? "on" : "off";

    const std::string_view unit = d.unit;
    if (unit == "deg") return formatNumber(value, 0) + "\xc2\xb0";
    if (unit == "deg/s") return formatNumber(value, 0) + " \xc2\xb0/s";
    if (unit == "dB") return formatNumber(value, 1) + " dB";
    if (unit == "Hz") return formatNumber(value, 2) + " hz";
    if (unit == "ms") return formatNumber(value, 0) + " ms";
    return formatNumber(value, 2);
}

}  // namespace bambi
