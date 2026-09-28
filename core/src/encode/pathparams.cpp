// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/encode/pathparams.hpp"

#include <array>

#include "bambi/mod/modulation.hpp"

namespace bambi {
namespace {

//  By shape, the parameter each setting index is, or kNoParamId.
constexpr std::array<ParamId, kMaxGenParams> keysFor(GeneratorType g) {
    std::array<ParamId, kMaxGenParams> keys{};
    for (int i = 0; i < kMaxGenParams; ++i) keys[static_cast<std::size_t>(i)] = pathParamId(g, i);
    return keys;
}

constexpr std::array<std::array<ParamId, kMaxGenParams>, 5> kKeys{
    keysFor(GeneratorType::Orbit), keysFor(GeneratorType::Lissajous), keysFor(GeneratorType::Wave),
    keysFor(GeneratorType::Arc), keysFor(GeneratorType::Spiral)};

const std::array<ParamId, kMaxGenParams>& keysOf(GeneratorType g) { return kKeys[static_cast<std::size_t>(g)]; }

constexpr ParamId kNone = kNoParamId;

}  // namespace

void pathSettingsFrom(TrajectoryState& s, std::span<const float> values) {
    const auto& keys = keysOf(s.generator);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto at = static_cast<std::size_t>(keys[i]);
        if (keys[i] != kNone && at < values.size()) s.genParams[i] = static_cast<double>(values[at]);
    }
}

void pathSettingsFrom(TrajectoryState& s, const ModulationEngine& mod) noexcept {
    const auto& keys = keysOf(s.generator);
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] != kNone) s.genParams[i] = mod.destination(keys[i]);
}

}  // namespace bambi
