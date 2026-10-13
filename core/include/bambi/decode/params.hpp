// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/blocks.hpp"
#include "bambi/patch/parameters.hpp"

/*  The decoder's parameter list. Never rename or remove a key; ranges, names and defaults may move,
 *  but a new parameter is appended at the end.
 *
 *  Short on purpose: a monitor has few knobs a host should automate. Yaw, pitch and roll and their
 *  rates turn the scene -- the whole field, for every output, and what an offline render turns it by;
 *  a tracker's orientation is an offset on top of them, never a parameter. The level is the output's.
 *  Everything else about the decoder -- what plays and where, the layout, the listener's two sliders,
 *  the weighting, the tracker's setup -- is state (DecoderState): it describes the listening situation
 *  and is never automated, so a bounce hears it whole and so does the worker that builds the filters
 *  from it. The shared blocks are every plugin's.
 */
namespace bambi {

// clang-format off
// group | id | key | display name | unit | type | min | max | default | choices
#define BAMBI_DECODER_PARAM_LIST(X)                                                                 \
  /*  The scene's turn, degrees: yaw left-positive, pitch up-positive, roll right-ear-down. */      \
  X(Transform, HeadYaw,   "head.yaw",   "yaw",   "deg", Float, -180, 180, 0, nullptr)             \
  X(Transform, HeadPitch, "head.pitch", "pitch", "deg", Float,  -90,  90, 0, nullptr)             \
  X(Transform, HeadRoll,  "head.roll",  "roll",  "deg", Float, -180, 180, 0, nullptr)             \
                                                                                                    \
  X(Render, OutputLevel,  "output.level", "level", "dB", Float, -60, 12, 0, nullptr)              \
                                                                                                    \
  /*  The one region slot every plugin carries; the decoder reads nothing from it.               */ \
  BAMBI_REGION_BLOCK(X, Region1, "region1", "region")                                               \
  X(Modulation, ModAmountRegion1, "mod.amount.region1", "region amount", "", Float, 0, 1, 1, nullptr) \
                                                                                                    \
  BAMBI_GENERATOR_BLOCK(X)                                                                          \
  BAMBI_MOD_AMOUNT_BLOCK(X)                                                                         \
  BAMBI_RATES_BLOCK(X)                                                                              \
  BAMBI_DETECTOR_BLOCK(X)                                                                           \
                                                                                                    \
  /*  The scene's turn by rate, degrees a second, integrated while the transport runs and wrapping; \
   *  after the shared blocks, since a key's place in the list is fixed once a host has seen it.   */ \
  X(Transform, HeadYawRate,   "head.yaw_rate",   "yaw rate",   "deg/s", Float, -180, 180, 0, nullptr) \
  X(Transform, HeadPitchRate, "head.pitch_rate", "pitch rate", "deg/s", Float, -180, 180, 0, nullptr) \
  X(Transform, HeadRollRate,  "head.roll_rate",  "roll rate",  "deg/s", Float, -180, 180, 0, nullptr)
// clang-format on

BAMBI_DEFINE_PARAM_IDS(DecoderParam, BAMBI_DECODER_PARAM_LIST)
BAMBI_DEFINE_PARAM_SPECS(decoder, BAMBI_DECODER_PARAM_LIST)
inline constexpr int kNumDecoderParams = DecoderParam::kCount;

/// The decoder's manifest: its descriptors and the side tables beside them.
const ParamManifest& decodeParams();
/// A parameter by its key, for the plugin's own code.
inline ParamId decoderParam(const char* key) { return static_cast<ParamId>(decodeParams().byKey(key)); }
/// What the shared modulation engine reads in the decoder: generators, source amounts, base targets.
const ModManifest& decodeMod();

}  // namespace bambi
