// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>

#include "bambi/math/vec3.hpp"

/*  Rotation matrices for an ambisonic field. A rotation never mixes orders, so it is one orthogonal
 *  (2n+1)-square block per order (its inverse is its transpose). Built by the Ivanic-Ruedenberg
 *  recurrence (J. Phys. Chem. 1996, with its 1998 erratum), without allocation.
 */
namespace bambi {

/// A rotation of space, row-major: `R * v` is `{r[0]*x + r[1]*y + r[2]*z, ...}`.
using Mat3 = std::array<double, 9>;

constexpr Mat3 kIdentity3{1, 0, 0, 0, 1, 0, 0, 0, 1};

Vec3 apply(const Mat3& r, Vec3 v);
Mat3 multiply(const Mat3& a, const Mat3& b);  ///< a after b
Mat3 transposed(const Mat3& r);

/// The rotation by `angleRad` about a unit `axis`, right-handed.
Mat3 rotationAbout(Vec3 axis, double angleRad);

/// A rotation carrying `axis` onto +z. Not unique: any turn about z may follow it, so what is done
/// between it and its transpose must commute with such a turn.
Mat3 axisToPole(Vec3 axis);

/// How many numbers the blocks for orders 0..order take: the sum of (2n+1)^2.
constexpr int rotationSize(int order) { return (order + 1) * (2 * order + 1) * (2 * order + 3) / 3; }

/// Where order n's block starts in that array. The block is row-major, 2n+1 square, ACN within it.
constexpr int rotationOffset(int n) { return n == 0 ? 0 : rotationSize(n - 1); }

/// The blocks that move every source in a field from d to R*d, orders 0..order. Real-time safe.
void shRotation(const Mat3& r, int order, std::span<double> blocks) noexcept;

/// Apply the blocks to one frame of numChannels(order) channels. `in` and `out` must not overlap.
void rotateSH(std::span<const double> blocks, int order, std::span<const double> in, std::span<double> out) noexcept;

}  // namespace bambi
