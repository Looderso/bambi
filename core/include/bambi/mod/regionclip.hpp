// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/region/shape.hpp"

/// A region on the clipboard: text that travels between tabs, plugins, instances and projects as an
/// action, not a connection. Carries the region's own definition by field, not by key, so `yaw`
/// copied from Echo's send is `yaw` on Reverb's return; carries a driving LFO set to restart, keeping
/// its index as the sample-and-hold seed. Never carries the side or amount (the use's, not the
/// region's), rows from envelopes or from LFOs set to continue, or a rate's accumulated turn.
namespace bambi {

struct RegionClip {
    RegionShape shape;
    std::vector<std::pair<std::string, float>> fields;  ///< the region block's parameters by field, in their own units

    struct Lfo {
        int index{0};  ///< which LFO it was, and so which it becomes: the sample-and-hold seed
        float rate{0.0f}, sync{0.0f}, div{0.0f}, shape{0.0f}, phase{0.0f}, polarity{1.0f};
        float amount{1.0f};
    };
    std::vector<Lfo> lfos;

    struct Row {
        int lfo{0};
        std::string field;
        double depth{0.0};
    };
    std::vector<Row> rows;
};

RegionClip copyRegion(const ModManifest& mods, const PluginState& s, int slot, std::string_view prefix);

/// Apply a clip to `slot`: state (the shape, matrix rows) is written into `state` for the caller to
/// commit as one undoable edit; host parameters are returned as (position, plain value) pairs, held
/// to range, for the caller to write through the host. Every LFO row onto this region is replaced by
/// the clip's; rows from features and envelopes are this instance's own and are left alone.
void pasteRegion(const ModManifest& mods, const RegionClip& clip, int slot, std::string_view prefix, PluginState& state,
                 std::vector<std::pair<int, float>>& writes);

std::string writeRegionClip(const RegionClip& clip);

std::optional<RegionClip> readRegionClip(
    std::string_view text);  ///< fails closed: anything but this exact version is nothing at all

std::string describeRegionClip(const RegionClip& clip);  ///< "band, 78 % soft" -- softness as its share of a half turn

}  // namespace bambi
