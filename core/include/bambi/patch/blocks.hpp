// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>

#include "bambi/patch/parameters.hpp"

/// Parameter blocks common to every plugin, stamped under each plugin's own prefix -- same keys,
/// names, ranges and defaults -- at whatever position a plugin's list has reached, since
/// everything is addressed by key through a manifest, not by position.

// ---- one region slot ------------------------------------------------------------------------
/// What a region is set to; its kind and its sector and dot counts are state, not parameters.
/// Keys are by position (`region1`, `region2`), display names by role ("send size", "return size").
// clang-format off
// P id prefix | k key prefix | n display-name prefix
#define BAMBI_REGION_BLOCK(X, P, k, n)                                                        \
  X(Region, P##Yaw,           k ".yaw",            n " yaw",            "deg",   Float, -180, 180,   0, nullptr) \
  X(Region, P##Pitch,         k ".pitch",          n " pitch",          "deg",   Float, -180, 180,   0, nullptr) \
  X(Region, P##Roll,          k ".roll",           n " roll",           "deg",   Float, -180, 180,   0, nullptr) \
  X(Region, P##YawRate,       k ".yaw_rate",       n " yaw rate",       "deg/s", Float, -180, 180,   0, nullptr) \
  X(Region, P##PitchRate,     k ".pitch_rate",     n " pitch rate",     "deg/s", Float, -180, 180,   0, nullptr) \
  X(Region, P##RollRate,      k ".roll_rate",      n " roll rate",      "deg/s", Float, -180, 180,   0, nullptr) \
  X(Region, P##Softness,      k ".softness",       n " softness",       "deg",   Float,    0, 180,  20, nullptr) \
  X(Region, P##Size,          k ".size",           n " size",           "deg",   Float,    0, 180,  45, nullptr) \
  X(Region, P##BandElevation, k ".band_elevation", n " band elevation", "deg",   Float,  -90,  90,   0, nullptr) \
  X(Region, P##Thickness,     k ".thickness",      n " thickness",      "deg",   Float,    0, 180,  30, nullptr) \
  X(Region, P##Fill,          k ".fill",           n " fill",           "",      Float,    0,   1, 0.5, nullptr) \
  X(Region, P##DotSize,       k ".dot_size",       n " dot size",       "deg",   Float,    0,  90,  20, nullptr) \
  X(Region, P##Side,          k ".side",           n " side",           "",      Choice,   0,   1,   0, "inside,outside") \
  /* clouds: appended, the seed being state; evolve is the one matrix target */ \
  X(Region, P##Coverage,      k ".coverage",       n " coverage",       "",      Float,    0,   1, 0.5, nullptr) \
  X(Region, P##Contrast,      k ".contrast",       n " contrast",       "",      Float,    0,   2, 0.6, nullptr) \
  X(Region, P##Detail,        k ".detail",         n " detail",         "",      Float,    0,   1, 0.5, nullptr) \
  X(Region, P##Evolve,        k ".evolve",         n " evolve",         "deg",   Float, -180, 180,   0, nullptr)
// clang-format on

// ---- whatever a rate turns ---------------------------------------------------------------------
/// One switch for everything integrated from a rate, under one key. `restart` holds while the
/// transport is stopped and resets at a play start, locate or loop, so a render repeats;
/// `continue` runs always, so a bounce no longer repeats what was heard. An LFO keeps its own copy.
// clang-format off
#define BAMBI_RATES_BLOCK(X) \
  X(Motion, RatesRetrigger, "rates.retrigger", "rates retrigger", "", Choice, 0, 1, 0, "restart,continue")
// clang-format on

// ---- the detectors --------------------------------------------------------------------------
/// How long level takes to fall, the input's and the sidechain's; the release of whatever Level
/// drives. Host parameters, so they automate; not matrix targets.
// clang-format off
#define BAMBI_DETECTOR_BLOCK(X) \
  X(Modulation, LevelRelease,   "level.release",    "level release",    "ms", Float, 20, 2000, 200, nullptr) \
  X(Modulation, ScLevelRelease, "sc_level.release", "sc level release", "ms", Float, 20, 2000, 200, nullptr)
// clang-format on

// ---- the generators: three LFOs and three envelopes ------------------------------------------
/// LFOs and envelopes are the same in every plugin. A generator's trigger is state, not a
/// parameter, and is not here. Amplitude and offset scale and shift the shape for a bounded target.
// clang-format off
#define BAMBI_LFO_BLOCK(X, P, k, n)                                                           \
  X(Lfo, P##Rate,      k ".rate",      n " rate",      "Hz",  Float, 0.01,   2, 0.25, nullptr) \
  X(Lfo, P##Sync,      k ".sync",      n " sync",      "",    Bool,     0,   1,    1, nullptr) \
  X(Lfo, P##Div,       k ".div",       n " div",       "",    Choice,   0,  15,    4, "8/1,8/1T,4/1,4/1T,2/1,2/1T,1/1,1/1T,1/2,1/2T,1/4,1/4T,1/8,1/8T,1/16,1/16T") \
  X(Lfo, P##Shape,     k ".shape",     n " shape",     "",    Choice,   0,   3,    0, "sine,triangle,saw,s&h") \
  X(Lfo, P##Phase,     k ".phase",     n " phase",     "deg", Float,    0, 360,    0, nullptr) \
  X(Lfo, P##Retrigger, k ".retrigger", n " retrigger", "",    Choice,   0,   1,    0, "restart,continue") \
  /*  One knob, not two: independent amplitude/offset could push a value outside [-1, 1]; instead
   *  `offset = -polarity`, `amplitude = 1 + polarity` pins the ceiling at 1 and sweeps the floor.
   *  Default 1: an LFO rests bipolar.                                                       */ \
  X(Lfo, P##Polarity,  k ".polarity",  n " polarity",  "",    Float,    0,   1,    1, nullptr)
// clang-format on

/*  A curve per timed stage, -1..1: `ease(u, k) = (1 - exp(-k*u)) / (1 - exp(-k))` reaches its
 *  target at exactly the labelled time for every k, so a real exponential never makes the printed
 *  millisecond a lie (k > 0 fast-then-slow, k < 0 slow-then-fast, k -> 0 linear, the default).
 *  Sustain has none, being a level not a time. Polarity is the LFO's knob, default 0: unipolar. */
// clang-format off
#define BAMBI_ENV_BLOCK(X, P, k, n)                                                           \
  X(Envelope, P##Attack,       k ".attack",        n " attack",        "ms", Float, 0.1, 2000,  10, nullptr) \
  X(Envelope, P##AttackCurve,  k ".attack_curve",  n " attack curve",  "",   Float,  -1,    1,   0, nullptr) \
  X(Envelope, P##Decay,        k ".decay",         n " decay",         "ms", Float, 0.1, 5000, 200, nullptr) \
  X(Envelope, P##DecayCurve,   k ".decay_curve",   n " decay curve",   "",   Float,  -1,    1,   0, nullptr) \
  X(Envelope, P##Sustain,      k ".sustain",       n " sustain",       "",   Float,   0,    1, 0.5, nullptr) \
  X(Envelope, P##Release,      k ".release",       n " release",       "ms", Float, 0.1, 8000, 400, nullptr) \
  X(Envelope, P##ReleaseCurve, k ".release_curve", n " release curve", "",   Float,  -1,    1,   0, nullptr) \
  X(Envelope, P##Polarity,     k ".polarity",      n " polarity",      "",   Float,   0,    1,   0, nullptr)
// clang-format on

/// All six generators, in the order the matrix lays its columns out. The encoder's equivalent
/// keys predate this block and stay where they are.
// clang-format off
#define BAMBI_GENERATOR_BLOCK(X)                                                              \
  BAMBI_LFO_BLOCK(X, Lfo1, "lfo1", "lfo 1")                                                   \
  BAMBI_LFO_BLOCK(X, Lfo2, "lfo2", "lfo 2")                                                   \
  BAMBI_LFO_BLOCK(X, Lfo3, "lfo3", "lfo 3")                                                   \
  BAMBI_ENV_BLOCK(X, Env1, "env1", "env 1")                                                   \
  BAMBI_ENV_BLOCK(X, Env2, "env2", "env 2")                                                   \
  BAMBI_ENV_BLOCK(X, Env3, "env3", "env 3")
// clang-format on

namespace bambi {

/// The value at mid-travel for a shared key, or 0 for linear: a time or rate whose useful choices
/// are the short or slow ones.
constexpr float sharedSkew(std::string_view key) {
    const bool lfo = key.starts_with("lfo"), env = key.starts_with("env");
    if (key == "level.release" || key == "sc_level.release") return 200.0f;
    if (lfo && key.ends_with(".rate")) return 0.25f;
    if (env && key.ends_with(".attack")) return 50.0f;
    if (env && key.ends_with(".decay")) return 300.0f;
    if (env && key.ends_with(".release")) return 500.0f;
    return 0.0f;
}

/// Whether a shared key is one turn of something circular, and so wraps: a region's three angles,
/// clouds' evolve, an LFO's phase, but not a rate, which is a speed rather than a place.
constexpr bool sharedWraps(std::string_view key) {
    if (key.starts_with("lfo")) return key.ends_with(".phase");
    if (!key.starts_with("region")) return false;
    return key.ends_with(".yaw") || key.ends_with(".pitch") || key.ends_with(".roll") || key.ends_with(".evolve");
}

/// How a shared key answers modulation, or NotModulatable: a region's placement, rates and
/// continuous shape settings, but not its kind, counts or side, nor a generator's or detector's.
constexpr DestKind sharedDestKind(std::string_view key) {
    if (!key.starts_with("region")) return DestKind::NotModulatable;
    if (key.ends_with(".yaw_rate") || key.ends_with(".pitch_rate") || key.ends_with(".roll_rate"))
        return DestKind::Rate;
    if (key.ends_with(".yaw") || key.ends_with(".pitch") || key.ends_with(".roll") || key.ends_with(".evolve"))
        return DestKind::DirectAngle;
    if (key.ends_with(".size") || key.ends_with(".softness") || key.ends_with(".band_elevation") ||
        key.ends_with(".thickness") || key.ends_with(".fill") || key.ends_with(".dot_size") ||
        key.ends_with(".coverage") || key.ends_with(".contrast") || key.ends_with(".detail"))
        return DestKind::DirectScalar;
    return DestKind::NotModulatable;
}

}  // namespace bambi

// ---- the matrix's own amounts ----------------------------------------------------------------
/// One automatable amount per source slot: cells cannot be automated because their list grows.
/// The eighteen non-region ones are the same everywhere; a plugin appends `mod.amount.regionN`
/// for each region slot it has.
// clang-format off
#define BAMBI_MOD_AMOUNT_BLOCK(X)                                                             \
  X(Modulation, ModGlobalAmount,   "mod.global_amount",    "global amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLevel,    "mod.amount.level",     "level amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountAttack,   "mod.amount.attack",    "attack amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountTonal,    "mod.amount.tonal",     "tonal amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLow,      "mod.amount.low",       "low amount",       "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountMid,      "mod.amount.mid",       "mid amount",       "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountHigh,     "mod.amount.high",      "high amount",      "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScLevel,  "mod.amount.sc_level",  "sc level amount",  "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScAttack, "mod.amount.sc_attack", "sc attack amount", "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScTonal,  "mod.amount.sc_tonal",  "sc tonal amount",  "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScLow,    "mod.amount.sc_low",    "sc low amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScMid,    "mod.amount.sc_mid",    "sc mid amount",    "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountScHigh,   "mod.amount.sc_high",   "sc high amount",   "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo1,     "mod.amount.lfo1",      "lfo 1 amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo2,     "mod.amount.lfo2",      "lfo 2 amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountLfo3,     "mod.amount.lfo3",      "lfo 3 amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv1,     "mod.amount.env1",      "env 1 amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv2,     "mod.amount.env2",      "env 2 amount",     "", Float, 0, 1, 1, nullptr) \
  X(Modulation, ModAmountEnv3,     "mod.amount.env3",      "env 3 amount",     "", Float, 0, 1, 1, nullptr)
// clang-format on
