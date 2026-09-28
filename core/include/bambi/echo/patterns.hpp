// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>

#include "bambi/patch/parameters.hpp"

/*  Starting points for the four taps. A pattern writes which taps are on, their steps and offsets,
 *  axis, spin, skew and feedback -- nothing else -- through `set` as normalised parameter values, so
 *  it automates and undoes like a hand edit. `Spiral` is a fresh instance.
 */
namespace bambi {

enum class EchoPattern { Even, PingPong, Spiral, Cascade, Single };

inline constexpr int kEchoPatternKeys = 9;  ///< keys a pattern writes per tap

void applyPattern(const ParamManifest& m, EchoPattern p, const std::function<void(int at, float normalised)>& set);

}  // namespace bambi
