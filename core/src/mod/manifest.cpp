// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/mod/manifest.hpp"

#include <algorithm>
#include <string>

#include "bambi/patch/state.hpp"

namespace bambi {

bool ModManifest::complete() const {
    if (params == nullptr || globalAmount == kNoParam) return false;
    for (const LfoParams& l : lfos)
        if (l.rate == kNoParam || l.sync == kNoParam || l.div == kNoParam || l.shape == kNoParam ||
            l.phase == kNoParam || l.retrigger == kNoParam || l.polarity == kNoParam)
            return false;
    for (const EnvParams& e : envelopes)
        if (e.attack == kNoParam || e.decay == kNoParam || e.sustain == kNoParam || e.release == kNoParam ||
            e.attackCurve == kNoParam || e.decayCurve == kNoParam || e.releaseCurve == kNoParam ||
            e.polarity == kNoParam)
            return false;
    for (const int a : sourceAmount)
        if (a == kNoParam) return false;
    for (const int b : baseTargets)
        if (b == kNoParam) return false;
    return true;
}

namespace {

int at(const ParamManifest& p, const std::string& key) { return p.byKey(key); }

std::string numbered(const char* stem, int n, const char* field) {
    return std::string(stem) + std::to_string(n) + "." + field;
}

}  // namespace

ModManifestOwner::ModManifestOwner(const ParamManifest& p, std::span<const char* const> baseKeys) {
    for (int i = 0; i < 3; ++i) {
        const auto k = static_cast<std::size_t>(i);
        lfos_[k] = {at(p, numbered("lfo", i + 1, "rate")),    at(p, numbered("lfo", i + 1, "sync")),
                    at(p, numbered("lfo", i + 1, "div")),     at(p, numbered("lfo", i + 1, "shape")),
                    at(p, numbered("lfo", i + 1, "phase")),   at(p, numbered("lfo", i + 1, "retrigger")),
                    at(p, numbered("lfo", i + 1, "polarity"))};
        envs_[k] = {at(p, numbered("env", i + 1, "attack")),        at(p, numbered("env", i + 1, "decay")),
                    at(p, numbered("env", i + 1, "sustain")),       at(p, numbered("env", i + 1, "release")),
                    at(p, numbered("env", i + 1, "attack_curve")),  at(p, numbered("env", i + 1, "decay_curve")),
                    at(p, numbered("env", i + 1, "release_curve")), at(p, numbered("env", i + 1, "polarity"))};
    }

    // A slot's amount is looked up by its name rather than derived from its position, since the
    // key list is append-only.
    for (int i = 0; i < kNumSources; ++i)
        amounts_[static_cast<std::size_t>(i)] = at(p, "mod.amount." + std::string(sourceName(i)));

    const std::size_t n = std::min(baseKeys.size(), base_.size());
    for (std::size_t i = 0; i < n; ++i) base_[i] = at(p, baseKeys[i]);

    m_ = ModManifest{&p, lfos_, envs_, amounts_, std::span<const int>(base_.data(), n), at(p, "mod.global_amount")};
}

}  // namespace bambi
