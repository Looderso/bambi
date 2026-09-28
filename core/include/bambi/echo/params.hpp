// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/blocks.hpp"
#include "bambi/patch/parameters.hpp"

/*  Echo's parameter list. Hosts save automation by key, so: never rename a key, never remove one,
 *  and append new parameters at the end.
 */
namespace bambi {

/*  One tap, stamped four times. Per-tap defaults are arguments. A free time opens at the synced
 *  time at 120 bpm (125 ms a sixteenth), so leaving sync moves nothing.
 */
// clang-format off
// P id prefix | k key prefix | n display-name prefix | on | steps | offset | az | el | spin
#define BAMBI_ECHO_TAP_BLOCK(X, P, k, n, dOn, dSteps, dOffset, dAz, dEl, dSpin)                   \
  X(Render, P##On,          k ".on",           n " on",           "",    Bool,     0,     1,  dOn, nullptr) \
  X(Render, P##Synced,      k ".synced",       n " synced",       "",    Bool,     0,     1,     1, nullptr) \
  X(Motion, P##Steps,       k ".steps",        n " steps",        "",    Choice,   1,    16, dSteps, "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16") \
  X(Motion, P##OffsetSteps, k ".offset_steps", n " offset steps", "",    Choice,   0,    16, dOffset, "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16") \
  X(Motion, P##Swing,       k ".swing",        n " swing",        "",    Float,    0,   0.9,     0, nullptr) \
  X(Motion, P##Ms,          k ".ms",           n " time",         "ms",  Float,   20,  2400, dSteps * 125, nullptr) \
  X(Motion, P##OffsetMs,    k ".offset_ms",    n " offset",       "ms",  Float,    0,  2400, dOffset * 125, nullptr) \
  X(Render, P##Level,       k ".level",        n " level",        "dB",  Float,  -40,     0,    -3, nullptr) \
  X(Render, P##Feedback,    k ".feedback",     n " feedback",     "dB",  Float,  -24,  -0.3,    -6, nullptr) \
  X(Transform, P##Az,       k ".az",           n " axis azimuth", "deg", Float, -180,   180,  dAz, nullptr) \
  X(Transform, P##El,       k ".el",           n " axis elevation", "deg", Float, -90,  90,   dEl, nullptr) \
  X(Transform, P##Spin,     k ".spin",         n " spin",         "deg", Float, -180,   180, dSpin, nullptr) \
  X(Transform, P##Skew,     k ".skew",         n " skew",         "",    Float, -0.6,   0.6,     0, nullptr) \
  X(Transform, P##Blur,     k ".blur",         n " blur",         "deg", Float,    0,   180,     6, nullptr) \
  X(Render, P##LowCut,      k ".low_cut",      n " low cut",      "Hz",  Float,   20,  2000,    80, nullptr) \
  X(Render, P##HighCut,     k ".high_cut",     n " high cut",     "Hz",  Float,  500, 20000, 14000, nullptr)

// group | id | key | display name | unit | type | min | max | default | choices
#define BAMBI_ECHO_PARAM_LIST(X)                                                                  \
  BAMBI_ECHO_TAP_BLOCK(X, Tap1, "tap1", "tap 1", 1, 2, 0,  0, 90,   45)                           \
  BAMBI_ECHO_TAP_BLOCK(X, Tap2, "tap2", "tap 2", 0, 2, 1,  0, 90,  -45)                           \
  BAMBI_ECHO_TAP_BLOCK(X, Tap3, "tap3", "tap 3", 0, 4, 0, 90,  0,   60)                           \
  BAMBI_ECHO_TAP_BLOCK(X, Tap4, "tap4", "tap 4", 0, 3, 0,  0, 90,  180)                           \
                                                                                                   \
  X(Render, OutputWet, "output.wet", "wet", "dB", Float, -60, 12,   0, nullptr)                      \
  X(Render, OutputDry, "output.dry", "dry", "dB", Float, -60, 12,   0, nullptr)                      \
                                                                                                   \
  BAMBI_REGION_BLOCK(X, Region1, "region1", "send")                                               \
  X(Modulation, ModAmountRegion1, "mod.amount.region1", "send amount", "", Float, 0, 1, 1, nullptr) \
                                                                                                   \
  BAMBI_GENERATOR_BLOCK(X)                                                                         \
  BAMBI_MOD_AMOUNT_BLOCK(X)                                                                        \
  BAMBI_RATES_BLOCK(X)                                                            \
  BAMBI_DETECTOR_BLOCK(X)
// clang-format on

BAMBI_DEFINE_PARAM_IDS(EchoParam, BAMBI_ECHO_PARAM_LIST)

inline constexpr int kNumEchoParams = EchoParam::kCount;

BAMBI_DEFINE_PARAM_SPECS(echo, BAMBI_ECHO_PARAM_LIST)

const ParamManifest& echoParams();

const ModManifest& echoMod();

}  // namespace bambi
