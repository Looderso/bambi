// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/presets.hpp"

#include <array>

#include "bambi/reverb/control.hpp"

namespace bambi {
namespace {
template <ReverbPreset Room>
void room(const ParamManifest& m, PluginState& s) {
    applyPreset(
        m, Room,
        [&](int at, float normalised) { s.params[static_cast<std::size_t>(at)] = fromNormalised(m, at, normalised); },
        s.room);
}

constexpr std::array<FactoryPreset, 6> kPresets{{
    {"rooms/ambience", room<ReverbPreset::Ambience>},
    {"rooms/room", room<ReverbPreset::Room>},
    {"rooms/chamber", room<ReverbPreset::Chamber>},
    {"rooms/hall", room<ReverbPreset::Hall>},
    {"rooms/large hall", room<ReverbPreset::LargeHall>},
    {"rooms/cathedral", room<ReverbPreset::Cathedral>},
}};
}  // namespace

std::span<const FactoryPreset> reverbFactoryPresets() { return kPresets; }

}  // namespace bambi
