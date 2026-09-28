// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "bambi/region/shape.hpp"

/*  A region applied to a whole field, as a matrix on the channels (g is the region's value):
 *
 *      M_ij = (2 n_j + 1) / 4 pi  *  integral of  g(d) Y_i(d) Y_j(d)  over the sphere          (SN3D)
 *
 *  Inside is M, outside I - M. Being band-limited, its edges ring slightly outside 0..1.
 *  Axial kinds (everywhere, spot, band) are one small block per m about the region's axis. Dense
 *  kinds contract the region's harmonics a_k up to order 2N with Gaunt's integrals, built once:
 *
 *      M_ij = sum_k  a_k * Gamma_kij,      Gamma_kij = (2 n_j + 1) / 4 pi * integral of Y_k Y_i Y_j
 *
 *  Every kind is rebuilt inside the control step, so an automated shape bounces as it was heard.
 *  The tests check it against valueAt() in shape.hpp.
 */
namespace bambi {

/// A region's own-frame harmonics w_k = (2 n_k + 1) / 4 pi * integral of g(d) Y_k(d), the weights a
/// region converted to custom starts from. Allocates.
void projectWeights(const Region& r, int order, std::span<double> out);

class RegionOperator {
public:
    /// Allocates. `maxFrames` is the most apply() will be handed at once.
    void prepare(int order, int maxFrames);

    /// Real-time safe. A turn or a side change rebuilds no shape.
    bool set(const Region& region) noexcept;

    /// The field inside (or outside) the region. `in` and `out` must not overlap.
    void apply(const float* in, float* out, int frames) noexcept;

    /// The last axial blocks, row-major, own frame. For tests and drawing.
    std::span<const double> blocks() const { return blocks_; }
    /// The own-frame matrix of a dense kind, row-major.
    std::span<const double> dense() const { return dense_; }
    bool isAxial() const { return axialKind_; }
    /// apply() copies the field unchanged: a refused kind, or "everywhere" from the inside.
    bool passesEverything() const { return refused_ || (identity_ && !outside_); }

private:
    int order_{0}, maxFrames_{0};
    bool identity_{true}, refused_{false}, plain_{true}, outside_{false}, axialKind_{true};
    Region denseFor_{};  ///< the shape `dense_` was built for, orientation aside
    bool denseValid_{false};
    std::vector<double> nodeZ_, nodeW_;  ///< Gauss-Legendre on [-1, 1], mapped onto each stretch
    std::vector<double> terms_, blocks_, rotation_, dense_;
    std::vector<float> axial_, denseF_, toOwn_, fromOwn_, a_, b_;

    //  ---- the dense kinds ----
    /*  Gamma by matrix entry: entry (i, j) sums `gauntV_[e] * a[gauntK_[e]]` over its run [begin, end),
     *  its k rising, so a run is summed in a register and written once. */
    struct GauntRun {
        std::uint16_t i, j;
        std::uint32_t begin, end;
    };
    /*  One dot set, in the frame that puts its first dot on the pole: every other dot as the colatitude
     *  and azimuth of the bisector it makes with the first, how far the first dot's cell reaches, and the
     *  sum of the rotations that carry that cell onto every cell -- orders 0..2N. */
    struct DotSet {
        int count{0}, others{0};
        std::array<double, 20> sepCos{}, sepSin{}, azimuth{};
        double cellReach{0.0};
        std::vector<double> rotationSum;
    };
    std::vector<GauntRun> gauntRuns_;
    std::vector<std::uint16_t> gauntK_;
    std::vector<double> gauntV_;
    std::vector<double> coef_, own_, high_;  ///< the region's harmonics to 2N, world and own-cell; SH at a node
    std::array<DotSet, 6> dotSets_{};
    std::vector<double> cosM_, sinM_;  ///< cos and sin of m x, m = 0..2N
    std::vector<double> phi_, fadeZ_, fadeW_, stretchZ_,
        stretchW_;  ///< a height's azimuthal integrals, m = -2N..2N; the two rules

    void buildDense(const Region& own) noexcept;
    void sectorHarmonics(const Region& own) noexcept;
    void dotHarmonics(const Region& own) noexcept;
    void prepareDense();
};

}  // namespace bambi
