// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>

namespace bambi {

/// What an engine lets in: a sample that is a number and under kInputLimit; anything else is silence. Written
/// out, since std::max and std::clamp pass a NaN through.
inline constexpr float kInputLimit = 1.0e6f;
inline float cleanSample(float v) noexcept { return (v == v && std::abs(v) < kInputLimit) ? v : 0.0f; }

}  // namespace bambi
