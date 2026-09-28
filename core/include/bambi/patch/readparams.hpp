// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "bambi/patch/parameters.hpp"
#include "bambi/region/shape.hpp"

/// Reads a plugin's settings out of its parameter values, by key through the manifest, into the
/// settings struct a control layer (`echo/control.hpp`, `reverb/control.hpp`) takes without
/// knowing parameter ids. A key a plugin does not have reads as the fallback.
namespace bambi {

/// Room for the longest "prefix.field" key. A longer key would be truncated, not found, and read
/// as its fallback; test_blocks.cpp checks every key fits.
inline constexpr std::size_t kMaxKeyChars = 64;

/// Builds `prefix.field` with no allocation, avoiding the silent audio-thread `operator new` a
/// `std::string` risks past libc++'s short-string limit. The view points into `into`, owned by
/// the caller for the length of the call.
inline std::string_view keyOf(std::array<char, kMaxKeyChars>& into, std::string_view prefix, std::string_view field) {
    const std::size_t n = std::min(prefix.size(), into.size() - 2);
    std::copy_n(prefix.data(), n, into.data());
    into[n] = '.';
    const std::size_t f = std::min(field.size(), into.size() - n - 1);
    std::copy_n(field.data(), f, into.data() + n + 1);
    return {into.data(), n + 1 + f};
}

/// A parameter's value by key, or `fallback` when this plugin has no such key.
inline double readParam(const ParamManifest& m, std::span<const float> values, std::string_view key,
                        double fallback = 0.0) {
    const int at = m.byKey(key);
    return at == kNoParam || at >= static_cast<int>(values.size())
               ? fallback
               : static_cast<double>(values[static_cast<std::size_t>(at)]);
}

/// The same, as a whole number: a Choice or a Bool.
inline int readChoice(const ParamManifest& m, std::span<const float> values, std::string_view key, int fallback = 0) {
    const int at = m.byKey(key);
    return at == kNoParam || at >= static_cast<int>(values.size())
               ? fallback
               : static_cast<int>(std::lround(static_cast<double>(values[static_cast<std::size_t>(at)])));
}

/// One region slot's settings. `prefix` is the slot: "region1", "region2". The shape -- kind and
/// counts -- is state, not read here; the caller brings it.
RegionSettingsDeg readRegionSettings(const ParamManifest& m, std::span<const float> values, std::string_view prefix);

/// That slot's rates, in degrees per second. A control step integrates them into a turn.
RegionRatesDeg readRegionRates(const ParamManifest& m, std::span<const float> values, std::string_view prefix);

/// Which side of that slot passes.
RegionSide readRegionSide(const ParamManifest& m, std::span<const float> values, std::string_view prefix);

}  // namespace bambi
