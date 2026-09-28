// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>

#include "bambi/patch/presets.hpp"

namespace bambi {

/// Reverb's factory presets: the six rooms, each built from the table `applyPreset` reads rather than
/// a copy of its numbers, so a room retuned there is retuned here.
std::span<const FactoryPreset> reverbFactoryPresets();

}  // namespace bambi
