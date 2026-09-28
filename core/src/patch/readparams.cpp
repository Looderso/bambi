// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/readparams.hpp"

namespace bambi {

RegionSettingsDeg readRegionSettings(const ParamManifest& m, std::span<const float> values, std::string_view prefix) {
    // Defaults come from the block itself, so an unstamped slot still reads as a valid region.
    const RegionSettingsDeg d;
    std::array<char, kMaxKeyChars> buf{};
    RegionSettingsDeg s;
    s.yaw = readParam(m, values, keyOf(buf, prefix, "yaw"), d.yaw);
    s.pitch = readParam(m, values, keyOf(buf, prefix, "pitch"), d.pitch);
    s.roll = readParam(m, values, keyOf(buf, prefix, "roll"), d.roll);
    s.softness = readParam(m, values, keyOf(buf, prefix, "softness"), d.softness);
    s.size = readParam(m, values, keyOf(buf, prefix, "size"), d.size);
    s.bandElevation = readParam(m, values, keyOf(buf, prefix, "band_elevation"), d.bandElevation);
    s.thickness = readParam(m, values, keyOf(buf, prefix, "thickness"), d.thickness);
    s.fill = readParam(m, values, keyOf(buf, prefix, "fill"), d.fill);
    s.dotSize = readParam(m, values, keyOf(buf, prefix, "dot_size"), d.dotSize);
    s.coverage = readParam(m, values, keyOf(buf, prefix, "coverage"), d.coverage);
    s.contrast = readParam(m, values, keyOf(buf, prefix, "contrast"), d.contrast);
    s.detail = readParam(m, values, keyOf(buf, prefix, "detail"), d.detail);
    s.evolve = readParam(m, values, keyOf(buf, prefix, "evolve"), d.evolve);
    return s;
}

RegionRatesDeg readRegionRates(const ParamManifest& m, std::span<const float> values, std::string_view prefix) {
    std::array<char, kMaxKeyChars> buf{};
    RegionRatesDeg r;
    r.yaw = readParam(m, values, keyOf(buf, prefix, "yaw_rate"), 0.0);
    r.pitch = readParam(m, values, keyOf(buf, prefix, "pitch_rate"), 0.0);
    r.roll = readParam(m, values, keyOf(buf, prefix, "roll_rate"), 0.0);
    return r;
}

RegionSide readRegionSide(const ParamManifest& m, std::span<const float> values, std::string_view prefix) {
    std::array<char, kMaxKeyChars> buf{};
    return readChoice(m, values, keyOf(buf, prefix, "side"), 0) > 0 ? RegionSide::Outside : RegionSide::Inside;
}

}  // namespace bambi
