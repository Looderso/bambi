// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/editor/PatchControls.h"

#include <algorithm>

#include "bambi/mod/matrix.hpp"

namespace bambi::editor {

namespace {
bool sameParams(const ParamManifest& m, const PluginState& a, const PluginState& b) {
    for (int i = 0; i < m.size(); ++i)
        if (!juce::exactlyEqual(a.params[static_cast<std::size_t>(i)], b.params[static_cast<std::size_t>(i)]))
            return false;
    return true;
}

bool sameMatrix(const PluginState& a, const PluginState& b) {
    if (a.matrix.size() != b.matrix.size()) return false;
    for (std::size_t i = 0; i < a.matrix.size(); ++i) {
        const auto& x = a.matrix[i];
        const auto& y = b.matrix[i];
        if (x.tab != y.tab || x.source != y.source || x.target != y.target || !juce::exactlyEqual(x.depth, y.depth))
            return false;
    }
    return true;
}

/*  The whole trigger and not its input alone: a gate or a threshold changed from another window, or
    by a state load, is drawn by the envelope page too. */
bool sameTriggers(const PluginState& a, const PluginState& b) { return a.envTriggers == b.envTriggers; }
}  // namespace

PatchControls::PatchControls(host::TargetHost& host, const ParamManifest& params, const ModManifest& mod, int ownTabs)
    : local(host),
      remoteTarget(host),
      patch(local.state()),
      params_(params),
      mod_(mod),
      ownTabs_(std::clamp(ownTabs, 1, kSourceTab)) {}

bool PatchControls::refresh() {
    //  whichever instance is selected; either target answers with its host parameters live
    auto next = target().state();
    const bool same = sameParams(params_, next, patch) && sameMatrix(next, patch) &&
                      next.trajectory == patch.trajectory && sameTriggers(next, patch) &&
                      next.regions == patch.regions && next.sources == patch.sources &&
                      next.renderQuality == patch.renderQuality;
    patch = std::move(next);
    if (provisional != kNoParamId && isRow(provisional)) {
        provisional = kNoParamId;  // it has a row of its own now: nothing left to hold open
        return true;
    }
    return !same;
}

void PatchControls::notify() const {
    if (changed) changed();
}

bool PatchControls::isRow(ParamId id) const {
    const auto rows = matrixTargets(mod_, patch);
    return std::find(rows.begin(), rows.end(), id) != rows.end();
}

void PatchControls::edit(std::string_view name, const UndoStack::Edit& change) {
    target().edit(name, change);
    refresh();
    notify();
}

void PatchControls::editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) {
    target().editDrag(name, key, change);
    refresh();
    notify();
}

void PatchControls::stopLearning() {
    if (target().learning() >= 0) target().learn(-1);
}

void PatchControls::openSource(int slot) {
    if (slot == source && tab == kSourceTab) {
        closeSource();
        return;
    }
    if (tab != kSourceTab) sourceReturn = tab;
    if (slot != source) stopLearning();  // learning belonged to the source that was open
    source = slot;
    tab = kSourceTab;
    notify();
}

void PatchControls::closeSource() {
    stopLearning();
    source = -1;
    tab = sourceReturn;
    notify();
}

void PatchControls::stepSource(int delta) {
    if (source < 0) return;
    const auto count = regionIsSource() ? kNumSources : 3 * kSourcesPerTab;
    const int next = ((source + delta) % count + count) % count;
    matrixTab = static_cast<MatrixTab>(next / kSourcesPerTab);
    //  not openSource(): stepping onto the source already open would close it
    if (next != source) stopLearning();
    source = next;
    tab = kSourceTab;
    notify();
}

void PatchControls::showTab(int index) {
    if (source >= 0) stopLearning();
    source = -1;
    tab = std::clamp(index, 0, ownTabs_ - 1);
    notify();
}

}  // namespace bambi::editor
