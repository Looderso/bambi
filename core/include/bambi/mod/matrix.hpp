// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
#include <vector>

#include "bambi/mod/manifest.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"

namespace bambi {

/// The modulation matrix as the editor sees and edits it. Pure functions on PluginState, so the
/// editor applies them to a copy and commits the result through the undo stack. Three tabs share one
/// list of target rows; a row exists while its target has depth on any tab, in the order it was
/// first given some.

struct SourceColumn {            ///< one column of a matrix tab: six on each
    std::string_view name;       ///< "level", "lfo 1"
    ParamId amount{kNoParamId};  ///< its automatable amount; none if absent
    bool bipolar{false};         ///< the LFOs; features and envelopes are unipolar
};
SourceColumn sourceColumn(const ModManifest& m, MatrixTab tab, int column);

bool isModulationTarget(const ModManifest& m, ParamId id);  ///< whether a matrix row can target it at all

/// The plugin's base targets, then every other target with depth, in the order it was first given
/// depth. Not a claim that anything is routed -- `targetHasDepth` answers that.
std::vector<ParamId> matrixTargets(const ModManifest& m, const PluginState& s);

bool targetHasDepth(const PluginState& s, ParamId target);  ///< driven from any source on any tab

double cellDepth(const PluginState& s, MatrixTab tab, int column, ParamId target);

/// Set one cell's depth, clamped to [-1, 1]; zero removes the cell. Returns false and changes nothing
/// for a parameter that is not a target or a column that does not exist.
bool setCellDepth(const ModManifest& m, PluginState& s, MatrixTab tab, int column, ParamId target, double depth);

bool hasDepthElsewhere(const PluginState& s, ParamId target,
                       MatrixTab tab);  ///< depth on another tab: the canvas's "elsewhere" dot

/// How far modulation can move a target from its base, in the parameter's own units, before clamping:
/// the same arithmetic as ModulationEngine::process, so a control can draw the range a source will
/// actually cover.
struct Reach {
    double low{0.0};
    double high{0.0};
};
Reach modulationReach(const ModManifest& m, const PluginState& s, ParamId target);

}  // namespace bambi
