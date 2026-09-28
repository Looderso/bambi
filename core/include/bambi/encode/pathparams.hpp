// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>

#include "bambi/encode/params.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/shape.hpp"

/*  A parametric path's settings as host parameters. Its continuous settings are keys of their own, one
 *  per shape, so they automate and modulate; its whole-number ones -- ratios, turns, wobbles -- stay in
 *  state, since they jump rather than sweep. A path's continuous settings are read from state as set
 *  for what is drawn and saved, or from modulation for what plays.
 */
namespace bambi {

class ModulationEngine;

/// A continuous path setting and the parameter it is: setting `index` of `shape`'s generatorParams.
struct PathKey {
    ParamId id;
    GeneratorType shape;
    int index;
};

inline constexpr std::array<PathKey, 15> kPathKeys{{
    {EncoderParam::OrbitTilt, GeneratorType::Orbit, 0},
    {EncoderParam::OrbitAxisAz, GeneratorType::Orbit, 1},
    {EncoderParam::OrbitAperture, GeneratorType::Orbit, 2},
    {EncoderParam::LissajousAzAmount, GeneratorType::Lissajous, 0},
    {EncoderParam::LissajousElAmount, GeneratorType::Lissajous, 1},
    {EncoderParam::LissajousPhase, GeneratorType::Lissajous, 4},
    {EncoderParam::WaveElAmount, GeneratorType::Wave, 1},
    {EncoderParam::WaveElOffset, GeneratorType::Wave, 3},
    {EncoderParam::ArcCentreAz, GeneratorType::Arc, 0},
    {EncoderParam::ArcCentreEl, GeneratorType::Arc, 1},
    {EncoderParam::ArcLength, GeneratorType::Arc, 2},
    {EncoderParam::ArcHeading, GeneratorType::Arc, 3},
    {EncoderParam::SpiralFromEl, GeneratorType::Spiral, 1},
    {EncoderParam::SpiralToEl, GeneratorType::Spiral, 2},
    {EncoderParam::SpiralStartAz, GeneratorType::Spiral, 3},
}};

/// The path setting a parameter is, or null for any other parameter.
constexpr const PathKey* pathKeyOf(ParamId id) {
    for (const PathKey& k : kPathKeys)
        if (k.id == id) return &k;
    return nullptr;
}

/// The key of setting `index` of shape `g`, or kNoParamId for one that stays in state.
constexpr ParamId pathParamId(GeneratorType g, int index) {
    for (const PathKey& k : kPathKeys)
        if (k.shape == g && k.index == index) return k.id;
    return kNoParamId;
}

/// Each continuous setting of `s`'s shape read from `values` (kNumEncoderParams wide, as set); the rest left alone.
void pathSettingsFrom(TrajectoryState& s, std::span<const float> values);

/// The same, as the engine has them this step. Real-time safe.
void pathSettingsFrom(TrajectoryState& s, const ModulationEngine& mod) noexcept;

}  // namespace bambi
