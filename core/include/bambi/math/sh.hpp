// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>

#include "bambi/math/vec3.hpp"

namespace bambi {

/*  Real spherical harmonics — AmbiX: ACN channel ordering, SN3D normalisation.
 *
 *      ACN = n^2 + n + m       n = order, m = degree in [-n, n]
 *      SN3D N_n^m = sqrt( (2 - delta_m0) * (n-|m|)! / (n+|m|)! )
 */

/// The highest order the core supports: above every host's limit (VST3 buses stop at 7th order).
inline constexpr int kMaxOrder = 35;
inline constexpr int kMaxChannels = (kMaxOrder + 1) * (kMaxOrder + 1);

constexpr int numChannels(int order) { return (order + 1) * (order + 1); }

/// Order n of an ACN channel index.
constexpr int acnOrder(int acn) {
    int n = 0;
    while ((n + 1) * (n + 1) <= acn) ++n;
    return n;
}

/// Degree m of an ACN channel index, in [-n, n].
constexpr int acnDegree(int acn) {
    const int n = acnOrder(acn);
    return acn - n * n - n;
}

/// Writes numChannels(order) values into out; p must be a unit vector.
void shSN3D(Vec3 p, int order, std::span<double> out);

/// Closed forms for order <= 3: an independent reference the tests check shSN3D against, which
/// catches sign errors no normalisation test can see.
void shSN3D_ref(Vec3 p, int order, std::span<double> out);

}  // namespace bambi
