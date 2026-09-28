// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/blocks.hpp"
#include "bambi/patch/parameters.hpp"

/*  Reverb's parameter list. Never rename or remove a key; ranges, names and defaults may move, but a
 *  new parameter is appended at the end.
 *
 *  Units are `reverb/control.hpp`'s -- metres, seconds, decibels, degrees, hertz -- since that is
 *  where a setting becomes a frame and this list fills its `ReverbSettings`.
 *
 *  Deliberately not here: the room's derived quantities (absorption, scattering, diffusion, damping,
 *  mixing time, line count, reflection order, tail order, modulation); the balance of reflections and
 *  tail, which follows from the room; `quality.render`, which is state rather than a parameter since
 *  it says what a bounce should do and a per-block automated value cannot answer that (it lives in
 *  `PluginState::renderQuality`); and ducking, whose shape the suite has not yet decided.
 */
namespace bambi {

// clang-format off
// group | id | key | display name | unit | type | min | max | default | choices
#define BAMBI_REVERB_PARAM_LIST(X)                                                                 \
  /*  The room: size and shape set when reflections arrive and how dense they are.                 */ \
  X(Render, RoomSize,      "room.size",      "size",      "m", Float,   3,  40,  20, nullptr)        \
  X(Render, RoomDecay,     "room.decay",     "decay",     "s", Float, 0.2,  10, 1.9, nullptr)        \
  X(Render, RoomTone,      "room.tone",      "tone",      "",  Float,   0,   1, 0.5, nullptr)        \
  X(Render, RoomRoughness, "room.roughness", "roughness", "",  Float,   0,   1, 0.6, nullptr)        \
  /*  A bus effect does not know its sources' distances, so it is one value.                      */ \
  X(Render, RoomDistance,  "room.distance",  "distance",  "m", Float, 0.5,  20,   6, nullptr)        \
                                                                                                    \
  /*  Pre-delay only adds to the one the geometry gives -- the derived one is already the first      \
   *  wall's arrival. Wet and dry are two levels, not a mix; -60 dB is silence, dry opens at unity. */  \
  X(Render, OutputWet,      "output.wet",       "wet",       "dB", Float, -60,  12,   0, nullptr)    \
  X(Render, OutputDry,      "output.dry",       "dry",       "dB", Float, -60,  12,   0, nullptr)    \
  X(Render, OutputPreDelay, "output.pre_delay", "pre-delay", "ms", Float,   0, 200,   0, nullptr)    \
                                                                                                    \
  X(Input, InputLowCut,  "input.low_cut",  "low cut",  "Hz", Float,   20,   500,   150, nullptr)     \
  X(Input, InputHighCut, "input.high_cut", "high cut", "Hz", Float, 2000, 20000, 11000, nullptr)     \
                                                                                                    \
  /*  Efficient or realistic while playing; the tail's line count is the whole of what this scales. */ \
  X(Render, QualityPlaying, "quality.playing", "quality", "", Choice, 0, 1, 1, "efficient,realistic") \
                                                                                                    \
  /*  Two slots, named by role.                                                                   */ \
  BAMBI_REGION_BLOCK(X, Region1, "region1", "send")                                                \
  X(Modulation, ModAmountRegion1, "mod.amount.region1", "send amount", "", Float, 0, 1, 1, nullptr)  \
  BAMBI_REGION_BLOCK(X, Region2, "region2", "return")                                              \
  X(Modulation, ModAmountRegion2, "mod.amount.region2", "return amount", "", Float, 0, 1, 1, nullptr) \
                                                                                                    \
  BAMBI_GENERATOR_BLOCK(X)                                                                          \
  BAMBI_MOD_AMOUNT_BLOCK(X)                                                                         \
  BAMBI_RATES_BLOCK(X)                                                            \
  BAMBI_DETECTOR_BLOCK(X)
// clang-format on

BAMBI_DEFINE_PARAM_IDS(ReverbParam, BAMBI_REVERB_PARAM_LIST)
BAMBI_DEFINE_PARAM_SPECS(reverb, BAMBI_REVERB_PARAM_LIST)

inline constexpr int kNumReverbParams = ReverbParam::kCount;

/// Reverb's manifest: its descriptors and the side tables beside them.
const ParamManifest& reverbParams();

/// What the shared modulation engine reads in Reverb: generators, source amounts, base targets.
const ModManifest& reverbMod();

}  // namespace bambi
