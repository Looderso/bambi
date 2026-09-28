// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/mod/matrix.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace bambi {

SourceColumn sourceColumn(const ModManifest& m, MatrixTab tab, int column) {
    static constexpr std::array<std::string_view, kSourcesPerTab> kFeatures{"level", "attack", "tonal",
                                                                            "low",   "mid",    "high"};
    static constexpr std::array<std::string_view, kSourcesPerTab> kGenerators{"lfo 1", "lfo 2", "lfo 3",
                                                                              "env 1", "env 2", "env 3"};
    if (column < 0 || column >= columnsIn(tab)) return {};
    const auto c = static_cast<std::size_t>(column);
    SourceColumn out;
    out.name = tab == MatrixTab::Region       ? std::string_view{"region"}
               : tab == MatrixTab::Generators ? kGenerators[c]
                                              : kFeatures[c];
    const int at = m.amountOf(sourceSlot(tab, column));
    out.amount = at == kNoParam ? kNoParamId : static_cast<ParamId>(at);
    out.bipolar = tab == MatrixTab::Generators && column < kNumLfos;
    return out;
}

bool isModulationTarget(const ModManifest& m, ParamId id) {
    return id != kNoParamId && m.params != nullptr &&
           m.params->destKindOf(static_cast<int>(id)) != DestKind::NotModulatable;
}

std::vector<ParamId> matrixTargets(const ModManifest& m, const PluginState& s) {
    std::vector<ParamId> out;
    out.reserve(m.baseTargets.size() + s.matrix.size());
    for (const int b : m.baseTargets) out.push_back(static_cast<ParamId>(b));
    for (const auto& c : s.matrix) {
        if (c.depth == 0.0 || !isModulationTarget(m, c.target)) continue;
        if (std::find(out.begin(), out.end(), c.target) == out.end()) out.push_back(c.target);
    }
    return out;
}

bool targetHasDepth(const PluginState& s, ParamId target) {
    return std::any_of(s.matrix.begin(), s.matrix.end(),
                       [&](const MatrixCell& c) { return c.target == target && c.depth != 0.0; });
}

double cellDepth(const PluginState& s, MatrixTab tab, int column, ParamId target) {
    for (const auto& c : s.matrix)
        if (c.tab == tab && c.source == column && c.target == target) return c.depth;
    return 0.0;
}

bool setCellDepth(const ModManifest& m, PluginState& s, MatrixTab tab, int column, ParamId target, double depth) {
    if (!isModulationTarget(m, target) || column < 0 || column >= columnsIn(tab)) return false;
    const double d = std::isfinite(depth) ? clampd(depth, -1.0, 1.0) : 0.0;

    //  One cell per (tab, column, target): update the first, and drop any duplicate a hand-edited
    //  document may carry, so the depth shown is the depth the engine sums.
    bool placed = false;
    for (auto it = s.matrix.begin(); it != s.matrix.end();) {
        const bool same = it->tab == tab && it->source == column && it->target == target;
        if (same && !placed && d != 0.0) {
            it->depth = d;
            placed = true;
            ++it;
        } else if (same) {
            it = s.matrix.erase(it);
        } else {
            ++it;
        }
    }
    if (!placed && d != 0.0) s.matrix.push_back({tab, column, target, d});
    return true;
}

bool hasDepthElsewhere(const PluginState& s, ParamId target, MatrixTab tab) {
    return std::any_of(s.matrix.begin(), s.matrix.end(),
                       [&](const MatrixCell& c) { return c.target == target && c.tab != tab && c.depth != 0.0; });
}

Reach modulationReach(const ModManifest& m, const PluginState& s, ParamId target) {
    Reach r;
    if (!isModulationTarget(m, target)) return r;
    const auto& d = (*m.params)[static_cast<int>(target)];
    const double scale =
        static_cast<double>(s.params[static_cast<std::size_t>(m.globalAmount)]) * static_cast<double>(d.max - d.min);
    for (const auto& c : s.matrix) {
        if (c.target != target || c.depth == 0.0) continue;
        const SourceColumn column = sourceColumn(m, c.tab, c.source);
        if (column.amount == kNoParamId) continue;
        const double k = c.depth * static_cast<double>(s.params[static_cast<std::size_t>(column.amount)]) * scale;
        r.low += column.bipolar ? -std::abs(k) : std::min(0.0, k);
        r.high += column.bipolar ? std::abs(k) : std::max(0.0, k);
    }
    return r;
}

}  // namespace bambi
