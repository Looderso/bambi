// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>

/*  Float kernels that transform an ambisonic field in place or frame by frame: what an effect's inner
 *  loop is made of. Fields are interleaved (data[f * channels + c], ACN), which is 2.8x faster than
 *  planar at order 7. Orders up to kFixedOrder are compiled with fixed sizes so the loops vectorise.
 *  Every kernel gives the same bits however the frames are split.
 */
namespace bambi {

inline constexpr int kFixedOrder = 7;

/// A rotation's blocks (math/shrotation.hpp) as float column-major blocks, and their inverse
/// (the transpose). Both are rotationSize(order) long.
void fieldBlocks(std::span<const double> blocks, int order, std::span<float> forward,
                 std::span<float> inverse) noexcept;

/// Turn `frames` frames by column-major blocks. `in` and `out` must not overlap.
void rotateField(std::span<const float> columnMajorBlocks, int order, const float* in, float* out, int frames) noexcept;

/// Turn a field about the pole, in place. `cosM[m]`, `sinM[m]` are of m times the angle, m = 0..order.
void spinField(std::span<const float> cosM, std::span<const float> sinM, int order, float* data, int frames) noexcept;

/*  An operator symmetric about the pole (a skew along it, a cap or band about it) couples only
 *  channels of the same m, +m and -m alike: one (order - m + 1)-square block per m, rows and columns
 *  the orders m..order. 344 multiplies a frame at order 7 instead of 4096. */
constexpr int axialSize(int order) { return (order + 1) * (order + 2) * (2 * order + 3) / 6; }
constexpr int axialOffset(int order, int m) {
    int at = 0;
    for (int k = 0; k < m; ++k) at += (order - k + 1) * (order - k + 1);
    return at;
}

/// Apply column-major axial blocks. Non-empty `cosM`/`sinM` also turn the result about the pole, at
/// no extra cost. `in` and `out` must not overlap.
void applyAxial(std::span<const float> columnMajorBlocks, std::span<const float> cosM, std::span<const float> sinM,
                int order, const float* in, float* out, int frames) noexcept;

/// Row-major double blocks to the column-major float the kernels read.
void axialBlocks(std::span<const double> rowMajor, int order, std::span<float> columnMajor) noexcept;

/// One gain per order, in place: `gains[n]` on every channel of order n.
void orderGains(std::span<const float> gains, int order, float* data, int frames) noexcept;

}  // namespace bambi
