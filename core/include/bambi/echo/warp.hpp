// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

#include "bambi/dsp/field.hpp"

/*  Echo's skew: a field squeezed toward one pole and stretched away from the other. About the pole,
 *  a direction moves along its meridian:
 *
 *      z' = (z + a) / (1 + a z),        a = the skew, -1 < a < 1
 *
 *  The matrix composes the field with that map (a source at d becomes a source at T(d), unit gain):
 *
 *      M_ij = (2 n_j + 1) / 4 pi  *  integral of  Y_i(T d) Y_j(d)  over the sphere          (SN3D)
 *
 *  Its spectral radius is exactly 1. Azimuth is untouched, so it is one (N-m+1)-square block per m,
 *  shared by +m and -m -- which is why a tap turns its axis to the pole first. The integrand has a
 *  pole at z = -1/a, so it is summed on 64 Gauss-Legendre nodes: exact to 1e-12 for |a| <= 0.9 up to
 *  order 7.
 */
namespace bambi {

inline constexpr int kWarpNodes = 64;
inline constexpr double kMaxSkew = 0.9;

/// The map itself, on the cosine of colatitude. Shared with echo/ping.hpp.
constexpr double slideZ(double z, double skew) {
    const double a = skew < -kMaxSkew ? -kMaxSkew : (skew > kMaxSkew ? kMaxSkew : skew);
    return (z + a) / (1.0 + a * z);
}

/// Laid out as dsp/field.hpp's axial blocks: one per m, row-major.
constexpr int warpSize(int order) { return axialSize(order); }
constexpr int warpOffset(int order, int m) { return axialOffset(order, m); }

class WarpBuilder {
public:
    /// Allocates and tabulates everything independent of the skew. `nodes` is for tests.
    void prepare(int order, int nodes = kWarpNodes);

    /// The blocks for a skew, clamped to +-kMaxSkew, into warpSize(order) numbers. Real-time safe.
    void build(double skew, std::span<double> blocks) noexcept;

    int order() const { return order_; }

private:
    int order_{0};
    std::vector<double> z_, weight_;
    std::vector<double> atNode_;   ///< SN3D terms at each node, channel-indexed
    std::vector<double> atImage_;  ///< scratch
};

/// One frame of (order+1)^2 channels. `in` and `out` must not overlap.
void applyWarp(std::span<const double> blocks, int order, std::span<const double> in, std::span<double> out) noexcept;

}  // namespace bambi
