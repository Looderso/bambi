// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <string>
#include <string_view>

#include "bambi/mod/manifest.hpp"
#include "bambi/patch/blocks.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/path/generator.hpp"

/*  The encoder's parameter list: `patch/parameters.hpp` holds the shape of a manifest and the
 *  machinery that reads one; this file holds the encoder's actual list -- the X-macro, the
 *  enumerators it writes, and the tables beside them. Echo and Reverb keep their own lists, and
 *  nothing below `encode/` in the layering may name any of them.
 *
 *  A host persists automation against the key (JUCE hashes it into a VST3 ParamID), so a renamed
 *  key silently detaches every saved automation lane: never rename or remove a key, only append new
 *  ones at the end. Display names, ranges and defaults may change; keys may not --
 *  `test_parameters.cpp` carries the frozen list that enforces it.
 *
 *  Trajectory shape and matrix cell depths are state rather than parameters: shape changes invalidate
 *  the arc-length LUT, and cell depths would make the parameter list unbounded, so automation instead
 *  rides one amount per source.
 *
 *  The `ParamId` overloads at the bottom call the manifest form, so callers keep addressing a
 *  parameter by name while everything shared beneath addresses it by position.
 */
namespace bambi {

// clang-format off
// group | id | key | display name | unit | type | min | max | default | choices
#define BAMBI_ENCODER_PARAM_LIST(X)                                                                   \
  X(Transform, TransformYaw,    "transform.yaw",    "yaw",    "deg", Float, -180, 180, 0, nullptr) \
  X(Transform, TransformPitch,  "transform.pitch",  "pitch",  "deg", Float, -180, 180, 0, nullptr) \
  X(Transform, TransformRoll,   "transform.roll",   "roll",   "deg", Float, -180, 180, 0, nullptr) \
  X(Transform, TransformExtent, "transform.extent", "extent", "",    Float,    0,   2, 1, nullptr) \
                                                                                               \
  X(Motion, MotionSpeed,     "motion.speed",     "speed",     "deg/s", Float, -180, 180, 0, nullptr) \
  X(Motion, MotionDisplace,  "motion.displace",  "displace",  "",      Float,    0,   1, 0, nullptr) \
  X(Motion, MotionDirection, "motion.direction", "direction", "",      Choice,   0,   1, 0, "forward,reverse") \
  X(Motion, MotionMode,      "motion.mode",      "mode",      "",      Choice,   0,   2, 1, "wrap,ping-pong,once") \
                                                                                               \
  X(Render, RenderWidth,    "render.width",     "width",     "deg", Float,   0, 180,   0, nullptr) \
  X(Render, RenderWidthMin, "render.width_min", "width min", "deg", Float,   0, 180,   0, nullptr) \
  X(Render, RenderWidthMax, "render.width_max", "width max", "deg", Float,   0, 180, 120, nullptr) \
  X(Render, RenderGain,     "render.gain",      "gain",      "dB",  Float, -60,  12,   0, nullptr) \
                                                                                               \
  X(Input, InputTrim, "input.trim", "input trim", "dB", Float, -24, 24, 0, nullptr)             \
                                                                                               \
  X(Modulation, ModGlobalAmount,   "mod.global_amount",     "global amount",   "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLevel,    "mod.amount.level",      "level amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountAttack,   "mod.amount.attack",     "attack amount",   "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountTonal,    "mod.amount.tonal",      "tonal amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLow,      "mod.amount.low",        "low amount",      "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountMid,      "mod.amount.mid",        "mid amount",      "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountHigh,     "mod.amount.high",       "high amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScLevel,  "mod.amount.sc_level",   "sc level amount", "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScAttack, "mod.amount.sc_attack",  "sc attack amount","", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScTonal,  "mod.amount.sc_tonal",   "sc tonal amount", "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScLow,    "mod.amount.sc_low",     "sc low amount",   "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScMid,    "mod.amount.sc_mid",     "sc mid amount",   "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScHigh,   "mod.amount.sc_high",    "sc high amount",  "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo1,     "mod.amount.lfo1",       "lfo 1 amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo2,     "mod.amount.lfo2",       "lfo 2 amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo3,     "mod.amount.lfo3",       "lfo 3 amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv1,     "mod.amount.env1",       "env 1 amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv2,     "mod.amount.env2",       "env 2 amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv3,     "mod.amount.env3",       "env 3 amount",    "", Float, 0, 1, 1, nullptr) \
                                                                                               \
  X(Lfo, Lfo1Rate,  "lfo1.rate",  "lfo 1 rate",  "Hz",  Float, 0.01,  2, 0.25, nullptr)           \
  X(Lfo, Lfo1Sync,  "lfo1.sync",  "lfo 1 sync",  "",    Bool,     0,  1, 1, nullptr)           \
  X(Lfo, Lfo1Div,   "lfo1.div",   "lfo 1 div",   "",    Choice,   0, 15, 4, "8/1,8/1T,4/1,4/1T,2/1,2/1T,1/1,1/1T,1/2,1/2T,1/4,1/4T,1/8,1/8T,1/16,1/16T") \
  X(Lfo, Lfo1Shape, "lfo1.shape", "lfo 1 shape", "",    Choice,   0,  3, 0, "sine,triangle,saw,s&h") \
  X(Lfo, Lfo1Phase, "lfo1.phase", "lfo 1 phase", "deg", Float,    0,360, 0, nullptr)           \
  X(Lfo, Lfo2Rate,  "lfo2.rate",  "lfo 2 rate",  "Hz",  Float, 0.01,  2, 0.25, nullptr)           \
  X(Lfo, Lfo2Sync,  "lfo2.sync",  "lfo 2 sync",  "",    Bool,     0,  1, 1, nullptr)           \
  X(Lfo, Lfo2Div,   "lfo2.div",   "lfo 2 div",   "",    Choice,   0, 15, 4, "8/1,8/1T,4/1,4/1T,2/1,2/1T,1/1,1/1T,1/2,1/2T,1/4,1/4T,1/8,1/8T,1/16,1/16T") \
  X(Lfo, Lfo2Shape, "lfo2.shape", "lfo 2 shape", "",    Choice,   0,  3, 0, "sine,triangle,saw,s&h") \
  X(Lfo, Lfo2Phase, "lfo2.phase", "lfo 2 phase", "deg", Float,    0,360, 0, nullptr)           \
  X(Lfo, Lfo3Rate,  "lfo3.rate",  "lfo 3 rate",  "Hz",  Float, 0.01,  2, 0.25, nullptr)           \
  X(Lfo, Lfo3Sync,  "lfo3.sync",  "lfo 3 sync",  "",    Bool,     0,  1, 1, nullptr)           \
  X(Lfo, Lfo3Div,   "lfo3.div",   "lfo 3 div",   "",    Choice,   0, 15, 4, "8/1,8/1T,4/1,4/1T,2/1,2/1T,1/1,1/1T,1/2,1/2T,1/4,1/4T,1/8,1/8T,1/16,1/16T") \
  X(Lfo, Lfo3Shape, "lfo3.shape", "lfo 3 shape", "",    Choice,   0,  3, 0, "sine,triangle,saw,s&h") \
  X(Lfo, Lfo3Phase, "lfo3.phase", "lfo 3 phase", "deg", Float,    0,360, 0, nullptr)           \
                                                                                               \
  X(Envelope, Env1Attack,  "env1.attack",  "env 1 attack",  "ms", Float, 0.1, 2000,  10, nullptr) \
  X(Envelope, Env1Decay,   "env1.decay",   "env 1 decay",   "ms", Float, 0.1, 5000, 200, nullptr) \
  X(Envelope, Env1Sustain, "env1.sustain", "env 1 sustain", "",   Float,   0,    1, 0.5, nullptr) \
  X(Envelope, Env1Release, "env1.release", "env 1 release", "ms", Float, 0.1, 8000, 400, nullptr) \
  X(Envelope, Env2Attack,  "env2.attack",  "env 2 attack",  "ms", Float, 0.1, 2000,  10, nullptr) \
  X(Envelope, Env2Decay,   "env2.decay",   "env 2 decay",   "ms", Float, 0.1, 5000, 200, nullptr) \
  X(Envelope, Env2Sustain, "env2.sustain", "env 2 sustain", "",   Float,   0,    1, 0.5, nullptr) \
  X(Envelope, Env2Release, "env2.release", "env 2 release", "ms", Float, 0.1, 8000, 400, nullptr) \
  X(Envelope, Env3Attack,  "env3.attack",  "env 3 attack",  "ms", Float, 0.1, 2000,  10, nullptr) \
  X(Envelope, Env3Decay,   "env3.decay",   "env 3 decay",   "ms", Float, 0.1, 5000, 200, nullptr) \
  X(Envelope, Env3Sustain, "env3.sustain", "env 3 sustain", "",   Float,   0,    1, 0.5, nullptr) \
  X(Envelope, Env3Release, "env3.release", "env 3 release", "ms", Float, 0.1, 8000, 400, nullptr) \
                                                                                               \
  X(Transform, TransformYawRate,   "transform.yaw_rate",   "yaw rate",   "deg/s", Float, -180, 180, 0, nullptr) \
  X(Transform, TransformPitchRate, "transform.pitch_rate", "pitch rate", "deg/s", Float, -180, 180, 0, nullptr) \
  X(Transform, TransformRollRate,  "transform.roll_rate",  "roll rate",  "deg/s", Float, -180, 180, 0, nullptr) \
                                                                                               \
  X(Lfo,    Lfo1Retrigger,   "lfo1.retrigger",   "lfo 1 retrigger", "", Choice, 0, 1, 0, "restart,continue") \
  X(Lfo,    Lfo2Retrigger,   "lfo2.retrigger",   "lfo 2 retrigger", "", Choice, 0, 1, 0, "restart,continue") \
  X(Lfo,    Lfo3Retrigger,   "lfo3.retrigger",   "lfo 3 retrigger", "", Choice, 0, 1, 0, "restart,continue") \
  /*  replaces `motion.retrigger`, in the place that key held */                                     \
  BAMBI_RATES_BLOCK(X)                                                                           \
                                                                                               \
  BAMBI_REGION_BLOCK(X, Region1, "region1", "region")                                         \
  X(Modulation, ModAmountRegion1, "mod.amount.region1", "region amount", "", Float, 0, 1, 1, nullptr) \
                                                                                               \
  /*  One polarity knob replaces the pair `amplitude`/`offset`, whose combination could leave
   *  [-1, 1] outside the matrix's depth rule. Appended at the end since the list is append-only. */ \
  X(Lfo, Lfo1Polarity, "lfo1.polarity", "lfo 1 polarity", "", Float, 0, 1, 1, nullptr)          \
  X(Lfo, Lfo2Polarity, "lfo2.polarity", "lfo 2 polarity", "", Float, 0, 1, 1, nullptr)          \
  X(Lfo, Lfo3Polarity, "lfo3.polarity", "lfo 3 polarity", "", Float, 0, 1, 1, nullptr)          \
                                                                                                \
  /*  A curve per timed stage, and one polarity, per envelope. */                                    \
  X(Envelope, Env1AttackCurve,  "env1.attack_curve",  "env 1 attack curve",  "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env1DecayCurve,   "env1.decay_curve",   "env 1 decay curve",   "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env1ReleaseCurve, "env1.release_curve", "env 1 release curve", "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env1Polarity,     "env1.polarity",      "env 1 polarity",      "", Float,  0, 1, 0, nullptr) \
  X(Envelope, Env2AttackCurve,  "env2.attack_curve",  "env 2 attack curve",  "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env2DecayCurve,   "env2.decay_curve",   "env 2 decay curve",   "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env2ReleaseCurve, "env2.release_curve", "env 2 release curve", "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env2Polarity,     "env2.polarity",      "env 2 polarity",      "", Float,  0, 1, 0, nullptr) \
  X(Envelope, Env3AttackCurve,  "env3.attack_curve",  "env 3 attack curve",  "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env3DecayCurve,   "env3.decay_curve",   "env 3 decay curve",   "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env3ReleaseCurve, "env3.release_curve", "env 3 release curve", "", Float, -1, 1, 0, nullptr) \
  X(Envelope, Env3Polarity,     "env3.polarity",      "env 3 polarity",      "", Float,  0, 1, 0, nullptr) \
  /*  What a stereo input puts on the sphere. The mode is set once and takes no modulation; the
   *  spread and offset reshape smoothly, so they may move. Both stay on screen whatever the mode
   *  says, the one not in use greyed out.                                                       */ \
  X(Input, InputMode,   "input.mode",   "input mode",   "",    Choice, 0,   2,    0, "sum,mid/side,stereo") \
  X(Input, InputSpread, "input.spread", "input spread", "deg", Float,  0, 180,   90, nullptr) \
  X(Input, InputOffset, "input.offset", "input offset", "",    Float,  0, 0.5, 0.15, nullptr) \
                                                                                                \
  BAMBI_DETECTOR_BLOCK(X)                                                                       \
                                                                                                \
  /*  A parametric path's continuous settings, one key each, so they automate and modulate. A         \
   *  shape's whole-number settings -- ratios, turns, wobbles -- stay in state instead, since they      \
   *  jump rather than sweep. Ranges and defaults match the generator table; a test holds the two        \
   *  together.                                                                                  */ \
  X(Transform, OrbitTilt,          "orbit.tilt",          "orbit tilt",       "deg", Float, kOrbitParams[0].min, kOrbitParams[0].max, kOrbitParams[0].def, nullptr) \
  X(Transform, OrbitAxisAz,        "orbit.axis_az",       "orbit axis az",    "deg", Float, kOrbitParams[1].min, kOrbitParams[1].max, kOrbitParams[1].def, nullptr) \
  X(Transform, OrbitAperture,      "orbit.aperture",      "orbit aperture",   "deg", Float, kOrbitParams[2].min, kOrbitParams[2].max, kOrbitParams[2].def, nullptr) \
  X(Transform, LissajousAzAmount,  "lissajous.az_amount", "lissajous az amount",  "deg", Float, kLissajousParams[0].min, kLissajousParams[0].max, kLissajousParams[0].def, nullptr) \
  X(Transform, LissajousElAmount,  "lissajous.el_amount", "lissajous el amount",  "deg", Float, kLissajousParams[1].min, kLissajousParams[1].max, kLissajousParams[1].def, nullptr) \
  X(Transform, LissajousPhase,     "lissajous.phase",     "lissajous phase",      "deg", Float, kLissajousParams[4].min, kLissajousParams[4].max, kLissajousParams[4].def, nullptr) \
  X(Transform, WaveElAmount,       "wave.el_amount",      "wave el amount",  "deg", Float, kWaveParams[1].min, kWaveParams[1].max, kWaveParams[1].def, nullptr) \
  X(Transform, WaveElOffset,       "wave.el_offset",      "wave el offset",  "deg", Float, kWaveParams[3].min, kWaveParams[3].max, kWaveParams[3].def, nullptr) \
  X(Transform, ArcCentreAz,        "arc.centre_az",       "arc centre az",  "deg", Float, kArcParams[0].min, kArcParams[0].max, kArcParams[0].def, nullptr) \
  X(Transform, ArcCentreEl,        "arc.centre_el",       "arc centre el",  "deg", Float, kArcParams[1].min, kArcParams[1].max, kArcParams[1].def, nullptr) \
  X(Transform, ArcLength,          "arc.length",          "arc length",     "deg", Float, kArcParams[2].min, kArcParams[2].max, kArcParams[2].def, nullptr) \
  X(Transform, ArcHeading,         "arc.heading",         "arc heading",    "deg", Float, kArcParams[3].min, kArcParams[3].max, kArcParams[3].def, nullptr) \
  X(Transform, SpiralFromEl,       "spiral.from_el",      "spiral from el",    "deg", Float, kSpiralParams[1].min, kSpiralParams[1].max, kSpiralParams[1].def, nullptr) \
  X(Transform, SpiralToEl,         "spiral.to_el",        "spiral to el",      "deg", Float, kSpiralParams[2].min, kSpiralParams[2].max, kSpiralParams[2].def, nullptr) \
  X(Transform, SpiralStartAz,      "spiral.start_az",     "spiral start az",   "deg", Float, kSpiralParams[3].min, kSpiralParams[3].max, kSpiralParams[3].def, nullptr)
// clang-format on

BAMBI_DEFINE_PARAM_IDS(EncoderParam, BAMBI_ENCODER_PARAM_LIST)
BAMBI_DEFINE_PARAM_SPECS(encoder, BAMBI_ENCODER_PARAM_LIST)

inline constexpr int kNumEncoderParams = EncoderParam::kCount;

/// The encoder's manifest: its descriptors and the three side tables beside them.
const ParamManifest& encodeParams();

/// The shared modulation engine's view of it, every generator field bound by name (mod/manifest.hpp).
const ModManifest& encodeMod();

// ---- the encoder's own list, by ParamId ----------------------------------------------------------

inline const ParamDesc& parameter(ParamId id) { return encodeParams()[static_cast<int>(id)]; }
inline std::span<const ParamDesc> parameters() { return encodeParams().descs; }

/// Look up by stable key. kNoParamId when absent.
inline ParamId parameterByKey(std::string_view key) {
    const int at = encodeParams().byKey(key);
    return at == kNoParam ? kNoParamId : static_cast<ParamId>(at);
}

inline float clampToRange(ParamId id, float v) { return clampToRange(encodeParams(), static_cast<int>(id), v); }
inline float toNormalised(ParamId id, float v) { return toNormalised(encodeParams(), static_cast<int>(id), v); }
inline float fromNormalised(ParamId id, float v) { return fromNormalised(encodeParams(), static_cast<int>(id), v); }
inline int choiceCount(ParamId id) { return choiceCount(parameter(id)); }
inline std::string formatParameter(ParamId id, double v) { return formatParameter(parameter(id), v); }
inline DestKind destinationKind(ParamId id) { return encodeParams().destKindOf(static_cast<int>(id)); }
inline double destinationSmoothing(ParamId id) { return encodeParams().smoothingOf(static_cast<int>(id)); }

/// The amount parameter for a source slot. kNoParamId for a slot that does not exist.
inline ParamId sourceAmount(int slot) {
    const int at = encodeMod().amountOf(slot);
    return at == kNoParam ? kNoParamId : static_cast<ParamId>(at);
}

}  // namespace bambi
