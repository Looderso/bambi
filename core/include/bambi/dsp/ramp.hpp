// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace bambi {

/// How far along a ramp of `length` samples is after `elapsed` of them, clamped to 0..1.
constexpr double rampWeight(int elapsed, int length) { return std::min(1.0, elapsed / static_cast<double>(length)); }

/// A value moving linearly from `from` to `to` over one control hop. The caller owns the clock
/// and passes the weight (rampWeight) in, so the ramp is independent of how blocks are cut.
template <class T>
struct Ramp {
    T from{};
    T to{};

    /// The value at weight `w`, computed in the wider of T and the weight's type.
    template <class W = double>
    constexpr auto at(W w) const noexcept {
        return from + (to - from) * w;
    }

    /// Start a new hop toward `target` from where the last one had got to at `w`.
    template <class W = double>
    constexpr void glideTo(T target, W w) noexcept {
        from = static_cast<T>(at(w));
        to = target;
    }

    /// Start a new hop toward `target` from `start`.
    constexpr void glide(T start, T target) noexcept {
        from = start;
        to = target;
    }

    constexpr void jumpTo(T value) noexcept { from = to = value; }

    /// True while the ramp sits still at `value`, so a caller can skip the multiply.
    constexpr bool holds(T value) const noexcept { return from == value && to == value; }
};

}  // namespace bambi
