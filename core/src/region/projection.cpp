// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/region/projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "bambi/dsp/field.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"

namespace bambi {
namespace {

constexpr int kNodes = 32;  // per smooth stretch of the region's profile
/*  The dense kinds' two rules: a stretch of colatitude and an azimuthal fade, each taken in panels
 *  a wavelength of order 2N wide. Against rules four times as fine, the worst matrix entry over
 *  every kind, count and softness at orders 1-7 is 4e-6 off. */
constexpr int kStretchNodes = 12, kFadeNodes = 8;

bool axial(RegionKind k) { return k == RegionKind::Everywhere || k == RegionKind::Spot || k == RegionKind::Band; }

/*  The 2-D rule for a kind that is not axial: theta is Gauss-Legendre in z = cos(theta), in panels,
 *  so the polynomial half of the integrand is exact within a panel; phi is uniform, which is what a
 *  periodic integrand wants and which an edge in azimuth makes first-order.
 *
 *  Fixed rather than scaled by order, because the cost is the number of `valueAt` evaluations,
 *  theta x phi whatever the order. 64 x 256 puts the worst entry within 2e-4 of the definition at
 *  orders 1 and 3 and takes about a millisecond, which is why it happens off the audio thread. */
constexpr int thetaNodesFor(int) { return 64; }
constexpr int phiNodesFor(int) { return 256; }

/// The region's shape without its orientation or its side: what a dense matrix actually depends
/// on. For clouds, without the four settings either -- they are combined in, not built.
Region shapeOnly(const Region& r) {
    Region out = r;
    out.yaw = out.pitch = out.roll = 0.0;
    out.side = RegionSide::Inside;
    if (out.kind == RegionKind::Clouds) {
        const Region d;
        out.coverage = d.coverage, out.contrast = d.contrast, out.detail = d.detail, out.evolve = d.evolve;
    }
    return out;
}

}  // namespace

void RegionOperator::prepare(int order, int maxFrames) {
    order_ = order;
    maxFrames_ = maxFrames;
    gaussLegendre(kNodes, nodeZ_, nodeW_);
    terms_.assign(static_cast<std::size_t>(numChannels(order)), 0.0);
    blocks_.assign(static_cast<std::size_t>(axialSize(order)), 0.0);
    rotation_.assign(static_cast<std::size_t>(rotationSize(order)), 0.0);
    axial_.assign(blocks_.size(), 0.0f);
    //  The dense kinds: a full matrix, and what it is contracted from.
    const auto C = static_cast<std::size_t>(numChannels(order));
    dense_.assign(C * C, 0.0);
    denseF_.assign(C * C, 0.0f);
    denseValid_ = false;
    axialKind_ = true;
    toOwn_.assign(rotation_.size(), 0.0f);
    fromOwn_.assign(rotation_.size(), 0.0f);
    a_.assign(static_cast<std::size_t>(numChannels(order) * maxFrames), 0.0f);
    b_.assign(a_.size(), 0.0f);
    identity_ = true;
    refused_ = false;
    prepareDense();
}

void projectWeights(const Region& r, int order, std::span<double> out) {
    const int N = std::clamp(order, 0, kRegionWeightsOrder), C = numChannels(N);
    std::fill(out.begin(), out.end(), 0.0);
    const RegionField field(shapeOnly(r));
    std::vector<double> nodes, wts, y(static_cast<std::size_t>(C)), acc(static_cast<std::size_t>(C), 0.0);
    gaussLegendre(kNodes, nodes, wts);
    const int panels = thetaNodesFor(N) / kNodes, phis = phiNodesFor(N);
    const double panelHeight = 2.0 / panels, dPhi = 2.0 * kPi / phis;
    for (int panel = 0; panel < panels; ++panel) {
        const double lo = -1.0 + panelHeight * panel, mid = lo + panelHeight * 0.5, half = panelHeight * 0.5;
        for (std::size_t k = 0; k < nodes.size(); ++k) {
            const double z = mid + half * nodes[k], wz = wts[k] * half * dPhi;
            const double rad = std::sqrt(std::max(0.0, 1.0 - z * z));
            for (int p = 0; p < phis; ++p) {
                const double phi = (p + 0.5) * dPhi;
                const Vec3 d{rad * std::cos(phi), rad * std::sin(phi), z};
                const double g = field.at(d);
                if (g == 0.0) continue;
                shSN3D(d, N, y);
                for (int i = 0; i < C; ++i) acc[static_cast<std::size_t>(i)] += g * wz * y[static_cast<std::size_t>(i)];
            }
        }
    }
    for (int i = 0; i < C && i < static_cast<int>(out.size()); ++i)
        out[static_cast<std::size_t>(i)] = acc[static_cast<std::size_t>(i)] * (2.0 * acnOrder(i) + 1.0) / (4.0 * kPi);
}

namespace {

constexpr std::array<int, 6> kDotCounts{2, 4, 6, 8, 12, 20};

/// cos and sin of m x for m = 0..top, by recurrence from one of each.
void harmonicsOf(double x, int top, std::vector<double>& c, std::vector<double>& s) noexcept {
    const double c1 = std::cos(x), s1 = std::sin(x);
    c[0] = 1.0, s[0] = 0.0;
    for (int m = 1; m <= top; ++m) {
        c[static_cast<std::size_t>(m)] =
            c[static_cast<std::size_t>(m - 1)] * c1 - s[static_cast<std::size_t>(m - 1)] * s1;
        s[static_cast<std::size_t>(m)] =
            s[static_cast<std::size_t>(m - 1)] * c1 + c[static_cast<std::size_t>(m - 1)] * s1;
    }
}

/*  One dot's cell at a colatitude, in the frame with that dot on the pole: the azimuths left once
 *  every other dot's half is taken away. A point is nearer the pole's dot than dot j when
 *      cos t (1 - cos S_j)  >=  sin S_j sin t cos(phi - phi_j),
 *  so dot j takes an arc of half-width acos(cot t tan(S_j / 2)) around its azimuth, from t = S_j/2
 *  on. The gaps are written to `from`/`to`; the count is returned. */
struct Arcs {
    std::array<double, 48> from{}, to{};
};
int cellGaps(const std::array<double, 20>& sepCos, const std::array<double, 20>& sepSin,
             const std::array<double, 20>& azimuth, int others, double ct, double st, Arcs& gaps) noexcept {
    Arcs taken;
    int n = 0;
    for (int j = 0; j < others; ++j) {
        const auto J = static_cast<std::size_t>(j);
        const double a = ct * (1.0 - sepCos[J]), b = sepSin[J] * st;
        if (b < 1e-12) {
            if (a < 0.0) return 0;  // the far side of an opposite dot: nothing of this cell
            continue;
        }
        const double k = a / b;
        if (k >= 1.0) continue;
        if (k <= -1.0) return 0;
        const double half = std::acos(k);
        double s = azimuth[J] - half;
        s -= 2.0 * kPi * std::floor(s / (2.0 * kPi));
        const double e = s + 2.0 * half;
        if (e > 2.0 * kPi) {
            taken.from[static_cast<std::size_t>(n)] = s, taken.to[static_cast<std::size_t>(n++)] = 2.0 * kPi;
            taken.from[static_cast<std::size_t>(n)] = 0.0, taken.to[static_cast<std::size_t>(n++)] = e - 2.0 * kPi;
        } else {
            taken.from[static_cast<std::size_t>(n)] = s, taken.to[static_cast<std::size_t>(n++)] = e;
        }
    }
    for (int i = 1; i < n; ++i)  // by where they start; a handful, so by insertion
        for (int k = i; k > 0 && taken.from[static_cast<std::size_t>(k)] < taken.from[static_cast<std::size_t>(k - 1)];
             --k) {
            std::swap(taken.from[static_cast<std::size_t>(k)], taken.from[static_cast<std::size_t>(k - 1)]);
            std::swap(taken.to[static_cast<std::size_t>(k)], taken.to[static_cast<std::size_t>(k - 1)]);
        }
    int g = 0;
    double cursor = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s = taken.from[static_cast<std::size_t>(i)], e = taken.to[static_cast<std::size_t>(i)];
        if (s > cursor) gaps.from[static_cast<std::size_t>(g)] = cursor, gaps.to[static_cast<std::size_t>(g++)] = s;
        cursor = std::max(cursor, e);
    }
    if (cursor < 2.0 * kPi)
        gaps.from[static_cast<std::size_t>(g)] = cursor, gaps.to[static_cast<std::size_t>(g++)] = 2.0 * kPi;
    return g;
}

/// A rotation whose columns are a, b made perpendicular to it, and their cross product.
Mat3 frameOf(Vec3 a, Vec3 b) {
    const Vec3 p = unit(b - a * dot(a, b)), c = cross(a, p);
    return Mat3{a.x, p.x, c.x, a.y, p.y, c.y, a.z, p.z, c.z};
}

/// Whether r carries every one of `dirs` onto one of them.
bool keepsSet(const Mat3& r, std::span<const Vec3> dirs) {
    for (const Vec3& x : dirs) {
        const Vec3 y = bambi::apply(r, x);
        bool found = false;
        for (const Vec3& z : dirs) found = found || dot(y, z) > 1.0 - 1e-9;
        if (!found) return false;
    }
    return true;
}

}  // namespace

/// Builds Gamma and the dot sets. Allocates; everything the control step reads is sized here.
void RegionOperator::prepareDense() {
    const int N = order_, L = 2 * N, C = numChannels(N), K = numChannels(L);
    coef_.assign(static_cast<std::size_t>(K), 0.0);
    own_.assign(coef_.size(), 0.0);
    high_.assign(coef_.size(), 0.0);
    cosM_.assign(static_cast<std::size_t>(L + 1), 0.0);
    sinM_.assign(cosM_.size(), 0.0);
    phi_.assign(static_cast<std::size_t>(2 * L + 1), 0.0);
    gaussLegendre(kFadeNodes, fadeZ_, fadeW_);
    gaussLegendre(kStretchNodes, stretchZ_, stretchW_);

    //  GAUNT. The product of three harmonics is a polynomial in z of degree n_i + n_j + n_k at most
    //  once the sines pair up, so L + 2 Gauss nodes are exact; in azimuth it is a trigonometric
    //  polynomial of degree 4N, so 4L + 4 even samples are.
    std::vector<double> z, w;
    gaussLegendre(L + 2, z, w);
    std::vector<double> radial(z.size() * static_cast<std::size_t>(K)), y(static_cast<std::size_t>(K));
    for (std::size_t a = 0; a < z.size(); ++a) {
        shSN3D(Vec3{std::sqrt(std::max(0.0, 1.0 - z[a] * z[a])), 0.0, z[a]}, L, y);
        for (int k = 0; k < K; ++k) {
            const int n = acnOrder(k), m = k - n * n - n;
            radial[a * static_cast<std::size_t>(K) + static_cast<std::size_t>(k)] =
                y[static_cast<std::size_t>(n * n + n + std::abs(m))];
        }
    }
    const int P = 4 * L + 4, A = 2 * N + 1, B = 2 * L + 1;
    const auto T = [](int m, double x) { return m >= 0 ? std::cos(m * x) : std::sin(-m * x); };
    std::vector<double> around(static_cast<std::size_t>(A * A * B), 0.0);
    for (int a = -N; a <= N; ++a)
        for (int b = -N; b <= N; ++b)
            for (int c = -L; c <= L; ++c) {
                double acc = 0.0;
                for (int p = 0; p < P; ++p) {
                    const double x = 2.0 * kPi * p / P;
                    acc += T(a, x) * T(b, x) * T(c, x);
                }
                around[static_cast<std::size_t>(((a + N) * A + (b + N)) * B + (c + L))] = acc * 2.0 * kPi / P;
            }
    gauntRuns_.clear(), gauntK_.clear(), gauntV_.clear();
    for (int i = 0; i < C; ++i)
        for (int j = 0; j < C; ++j) {
            const auto begin = static_cast<std::uint32_t>(gauntK_.size());
            for (int k = 0; k < K; ++k) {
                const int ni = acnOrder(i), nj = acnOrder(j), nk = acnOrder(k);
                if (nk < std::abs(ni - nj) || nk > ni + nj || (ni + nj + nk) % 2 != 0) continue;
                const int mi = i - ni * ni - ni, mj = j - nj * nj - nj, mk = k - nk * nk - nk;
                const double az = around[static_cast<std::size_t>(((mi + N) * A + (mj + N)) * B + (mk + L))];
                if (std::abs(az) < 1e-12) continue;
                double zz = 0.0;
                for (std::size_t a = 0; a < z.size(); ++a) {
                    const double* r = radial.data() + a * static_cast<std::size_t>(K);
                    zz += w[a] * r[i] * r[j] * r[k];
                }
                const double v = zz * az * (2.0 * nj + 1.0) / (4.0 * kPi);
                if (std::abs(v) < 1e-15) continue;
                gauntK_.push_back(static_cast<std::uint16_t>(k));
                gauntV_.push_back(v);
            }
            const auto end = static_cast<std::uint32_t>(gauntK_.size());
            if (end > begin)
                gauntRuns_.push_back({static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(j), begin, end});
        }

    //  THE DOT SETS, each in the frame with its first dot on the pole.
    std::vector<double> turn(static_cast<std::size_t>(rotationSize(L)));
    for (std::size_t s = 0; s < kDotCounts.size(); ++s) {
        DotSet& ds = dotSets_[s];
        const auto dirs = dotDirections(kDotCounts[s]);
        ds.count = static_cast<int>(dirs.size());
        ds.others = ds.count - 1;
        ds.rotationSum.assign(turn.size(), 0.0);
        if (dirs.empty()) continue;
        const Mat3 toPole = axisToPole(dirs[0]);
        for (int j = 1; j < ds.count; ++j) {
            const Vec3 v = bambi::apply(toPole, dirs[static_cast<std::size_t>(j)]);
            const auto J = static_cast<std::size_t>(j - 1);
            ds.sepCos[J] = clampd(v.z, -1.0, 1.0);
            ds.sepSin[J] = std::sqrt(std::max(0.0, 1.0 - v.z * v.z));
            ds.azimuth[J] = std::atan2(v.y, v.x);
        }
        //  how far the cell reaches from its dot: the last colatitude with any of it left
        Arcs gaps;
        double lo = 0.0, hi = kPi;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            (cellGaps(ds.sepCos, ds.sepSin, ds.azimuth, ds.others, std::cos(mid), std::sin(mid), gaps) > 0 ? lo : hi) =
                mid;
        }
        ds.cellReach = lo;
        //  a symmetry of the set carrying the first dot onto each: it carries the first cell onto that dot's
        for (const Vec3& d : dirs) {
            Mat3 q = kIdentity3;
            if (ds.count == 2) {
                if (dot(d, dirs[0]) < 0.0) {
                    const Vec3 side = std::abs(d.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
                    q = rotationAbout(unit(cross(dirs[0], side)), kPi);
                }
            } else {
                std::size_t near = 1;
                for (std::size_t j = 2; j < dirs.size(); ++j)
                    if (dot(dirs[0], dirs[j]) > dot(dirs[0], dirs[near])) near = j;
                const double c0 = dot(dirs[0], dirs[near]);
                const Mat3 first = frameOf(dirs[0], dirs[near]);
                bool found = false;
                for (const Vec3& e : dirs) {
                    if (found || std::abs(dot(d, e) - c0) > 1e-9 || dot(d, e) > 1.0 - 1e-9) continue;
                    const Mat3 candidate = multiply(frameOf(d, e), transposed(first));
                    if (keepsSet(candidate, dirs)) q = candidate, found = true;
                }
            }
            shRotation(multiply(q, transposed(toPole)), L, turn);
            for (std::size_t k = 0; k < turn.size(); ++k) ds.rotationSum[k] += turn[k];
        }
    }
}

/// The dense matrix: the region's own harmonics to 2N, then Gamma. Bounded work, no allocation.
void RegionOperator::buildDense(const Region& own) noexcept {
    int weightsOrder = 2 * order_;  // the highest order of harmonic the kind can have
    switch (own.kind) {
        case RegionKind::Sectors: sectorHarmonics(own); break;
        case RegionKind::Dots: dotHarmonics(own); break;
        case RegionKind::Clouds:
        case RegionKind::Custom:
            //  a weights kind's harmonics are its weights; clouds' contrast is shared out over the
            //  orders the bus carries
            weightsOrder = own.kind == RegionKind::Clouds ? std::min(order_, kRegionWeightsOrder)
                                                          : std::min(2 * order_, kRegionWeightsOrder);
            regionWeights(own, weightsOrder, coef_);
            break;
        default: std::fill(coef_.begin(), coef_.end(), 0.0); break;
    }
    //  Summed a matrix entry at a time, in a register: one store an entry, not one a term. A weights
    //  kind's harmonics stop at its weights' order, and a run's k rises, so the rest of a run is zero.
    const int C = numChannels(order_);
    const auto top = static_cast<std::uint16_t>(numChannels(weightsOrder) - 1);
    std::fill(dense_.begin(), dense_.end(), 0.0);
    for (const GauntRun& run : gauntRuns_) {
        double sum = 0.0;
        for (std::uint32_t e = run.begin; e < run.end && gauntK_[e] <= top; ++e) sum += gauntV_[e] * coef_[gauntK_[e]];
        dense_[static_cast<std::size_t>(run.i) * static_cast<std::size_t>(C) + run.j] = sum;
    }
    for (std::size_t e = 0; e < dense_.size(); ++e) denseF_[e] = static_cast<float>(dense_[e]);
}

namespace {
/*  A stretch of colatitude, integrated with Gauss-Legendre under t = mid + half (3u - u^3) / 2: the
 *  substitution's derivative vanishes at both ends, which smooths the square-root corners a fade, a
 *  cell's edge and a cell's corner all put there. `at(t, weight)` is handed sin t dt already. */
template <class At>
void overStretch(double t0, double t1, int top, const std::vector<double>& z, const std::vector<double>& w,
                 At&& at) noexcept {
    if (t1 - t0 < 1e-14) return;
    //  In panels no wider than a wavelength of the top order the matrix needs: a stretch is smooth, but a
    //  harmonic of order 2N across all of it is more than one rule resolves.
    const int panels = std::max(1, static_cast<int>(std::ceil((t1 - t0) * (top + 2) / (2.0 * kPi))));
    const double width = (t1 - t0) / panels;
    for (int p = 0; p < panels; ++p) {
        const double mid = t0 + width * (p + 0.5), half = 0.5 * width;
        for (std::size_t k = 0; k < z.size(); ++k) {
            const double u = z[k], t = mid + half * (3.0 * u - u * u * u) * 0.5;
            at(t, w[k] * half * 1.5 * (1.0 - u * u) * std::sin(t));
        }
    }
}
}  // namespace

/*  Sectors, in their own frame: at each height the value is even in azimuth and repeats every
 *  sector, so only cos(m phi) with m a multiple of the count survives, and one half-sector's
 *  integral says it: flat to where the fade begins, in closed form; across the fade, a 16-point
 *  rule. The fade's reach in azimuth is r / sin t, so its ends meet 0 and the half-sector at
 *  heights of their own: those are cut. */
void RegionOperator::sectorHarmonics(const Region& own) noexcept {
    const int L = 2 * order_, n = std::max(own.sectors, 1);
    std::fill(coef_.begin(), coef_.end(), 0.0);
    const double period = 2.0 * kPi / n, half = period * 0.5, w = clampd(own.fill, 0.0, 1.0) * half;
    const bool hard = own.softness < kHardEdge;
    const double reach = own.softness * 0.5;
    //  The value depends on sin t alone, so it is the same either side of the equator: harmonics with n + m
    //  odd vanish, and the rest are twice the upper half's.
    std::array<double, 4> cuts{0.0, kPi * 0.5};
    int count = 2;
    if (!hard)
        for (const double x : {w, half - w})
            if (x > 0.0 && reach < x) cuts[static_cast<std::size_t>(count++)] = std::asin(reach / x);
    std::sort(cuts.begin(), cuts.begin() + count);

    std::array<double, 64> around{};  // the half-sector's cos(m u) integral, m = 0..L
    for (int c = 0; c + 1 < count; ++c)
        overStretch(cuts[static_cast<std::size_t>(c)], cuts[static_cast<std::size_t>(c + 1)], L, stretchZ_, stretchW_,
                    [&](double t, double weight) {
                        const double st = std::sin(t);
                        std::fill(around.begin(), around.begin() + L + 1, 0.0);
                        double lo = w, hi = w;
                        if (!hard) {
                            lo = st > 1e-12 ? clampd(w - reach / st, 0.0, half) : 0.0;
                            hi = st > 1e-12 ? clampd(w + reach / st, 0.0, half) : half;
                        }
                        harmonicsOf(lo, L, cosM_, sinM_);
                        around[0] = lo;
                        for (int m = n; m <= L; m += n)
                            around[static_cast<std::size_t>(m)] = sinM_[static_cast<std::size_t>(m)] / m;
                        //  across the fade, in panels no wider than a wavelength of the top order, as a stretch is
                        const int panels =
                            hi > lo ? std::max(1, static_cast<int>(std::ceil((hi - lo) * (L + 2) / (2.0 * kPi)))) : 0;
                        for (int p = 0; p < panels; ++p) {
                            const double fh = 0.5 * (hi - lo) / panels, fm = lo + fh * (2 * p + 1);
                            for (std::size_t f = 0; f < fadeZ_.size(); ++f) {
                                const double u = fm + fh * fadeZ_[f];
                                const double g = edgeProfile((w - u) * st, own.softness) * fadeW_[f] * fh;
                                if (g == 0.0) continue;
                                harmonicsOf(u, L, cosM_, sinM_);
                                for (int m = 0; m <= L; m += n)
                                    around[static_cast<std::size_t>(m)] += g * cosM_[static_cast<std::size_t>(m)];
                            }
                        }
                        shSN3D(Vec3{st, 0.0, std::cos(t)}, L, high_);
                        for (int m = 0; m <= L; m += n) {
                            const double c = weight * 4.0 * n * around[static_cast<std::size_t>(m)];  // both halves
                            for (int nn = m; nn <= L; nn += 2) {
                                const auto k = static_cast<std::size_t>(nn * nn + nn + m);
                                coef_[k] += c * high_[k];
                            }
                        }
                    });
    for (std::size_t k = 0; k < coef_.size(); ++k)
        coef_[k] *= (2.0 * acnOrder(static_cast<int>(k)) + 1.0) / (4.0 * kPi);
}

/*  Dots: one dot's cell, in the frame with it on the pole, where the dot is a function of colatitude
 *  alone and the cell's edge is arcs of azimuth in closed form (`cellGaps`). Every cell is that one
 *  turned by a symmetry of the set, so the region is the sum of the turns: one precomputed matrix. */
void RegionOperator::dotHarmonics(const Region& own) noexcept {
    std::fill(coef_.begin(), coef_.end(), 0.0);
    const DotSet* found = nullptr;
    for (const DotSet& ds : dotSets_)
        if (ds.count == own.dots && ds.count > 0) found = &ds;
    if (found == nullptr) return;
    const DotSet& ds = *found;
    const int L = 2 * order_;
    const bool hard = own.softness < kHardEdge;
    const double size = own.dotSize, reach = own.softness * 0.5;
    std::fill(own_.begin(), own_.end(), 0.0);

    std::array<double, 28> cuts{0.0, ds.cellReach};
    int count = 2;
    const auto cut = [&](double t) {
        if (t > 0.0 && t < ds.cellReach) cuts[static_cast<std::size_t>(count++)] = t;
    };
    if (hard)
        cut(size);
    else
        cut(size - reach), cut(size + reach);
    for (int j = 0; j < ds.others; ++j) cut(0.5 * std::acos(ds.sepCos[static_cast<std::size_t>(j)]));
    std::sort(cuts.begin(), cuts.begin() + count);

    Arcs gaps;
    for (int c = 0; c + 1 < count; ++c)
        overStretch(cuts[static_cast<std::size_t>(c)], cuts[static_cast<std::size_t>(c + 1)], L, stretchZ_, stretchW_,
                    [&](double t, double weight) {
                        const double e = size - t;
                        const double g = hard ? (e >= 0.0 ? 1.0 : 0.0) : edgeProfile(e, own.softness);
                        if (g == 0.0) return;
                        const double ct = std::cos(t), st = std::sin(t);
                        const int n = cellGaps(ds.sepCos, ds.sepSin, ds.azimuth, ds.others, ct, st, gaps);
                        if (n == 0) return;
                        std::fill(phi_.begin(), phi_.end(), 0.0);
                        for (int i = 0; i < n; ++i) {
                            const double a = gaps.from[static_cast<std::size_t>(i)],
                                         b = gaps.to[static_cast<std::size_t>(i)];
                            phi_[static_cast<std::size_t>(L)] += b - a;
                            harmonicsOf(b, L, cosM_, sinM_);
                            for (int m = 1; m <= L; ++m) {
                                phi_[static_cast<std::size_t>(L + m)] += sinM_[static_cast<std::size_t>(m)] / m;
                                phi_[static_cast<std::size_t>(L - m)] -= cosM_[static_cast<std::size_t>(m)] / m;
                            }
                            harmonicsOf(a, L, cosM_, sinM_);
                            for (int m = 1; m <= L; ++m) {
                                phi_[static_cast<std::size_t>(L + m)] -= sinM_[static_cast<std::size_t>(m)] / m;
                                phi_[static_cast<std::size_t>(L - m)] += cosM_[static_cast<std::size_t>(m)] / m;
                            }
                        }
                        shSN3D(Vec3{st, 0.0, ct}, L, high_);
                        for (int nn = 0; nn <= L; ++nn)
                            for (int m = -nn; m <= nn; ++m)
                                own_[static_cast<std::size_t>(nn * nn + nn + m)] +=
                                    weight * g * high_[static_cast<std::size_t>(nn * nn + nn + std::abs(m))] *
                                    phi_[static_cast<std::size_t>(L + m)];
                    });
    for (std::size_t k = 0; k < own_.size(); ++k) own_[k] *= (2.0 * acnOrder(static_cast<int>(k)) + 1.0) / (4.0 * kPi);
    rotateSH(ds.rotationSum, L, own_, coef_);
}

bool RegionOperator::set(const Region& region) noexcept {
    outside_ = region.side == RegionSide::Outside;
    /*  A kind that cannot be projected is `refused`, and passes the field whichever side was asked
     *  for -- silencing it would be an answer, and a wrong one. Everywhere is a real region: its
     *  outside is nothing. */
    refused_ = false;  //  every kind that exists is projected
    identity_ = region.kind == RegionKind::Everywhere;
    axialKind_ = axial(region.kind);
    if (identity_) return true;

    /*  A kind that is not axial: the dense matrix depends on the shape alone, so a region that is
     *  only turning -- which is what a modulated one does -- rebuilds nothing here and pays the
     *  same rotation an axial region pays. A shape that moves is rebuilt here, in bounded work. */
    if (!axialKind_) {
        Region shape = region;
        shape.yaw = shape.pitch = shape.roll = 0.0;
        shape.side = RegionSide::Inside;
        if (!denseValid_ || !(shape == denseFor_)) {
            buildDense(shape);
            denseFor_ = shape;
            denseValid_ = true;
        }
    }

    //  The region in its own frame, read from the inside: edgeDistance takes a direction already
    //  carried there, so its value at a height is the same on every meridian.
    if (axialKind_) {
        Region own = region;
        own.side = RegionSide::Inside;
        const auto valueAtHeight = [&](double z) {
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const double e = edgeDistance(own, Vec3{r, 0.0, z});
            return own.softness < kHardEdge ? (e >= 0.0 ? 1.0 : 0.0) : edgeProfile(e, own.softness);
        };

        /*  The profile is smooth between its edges and not across them, so it is integrated a stretch at
     *  a time: the heights where a fade begins and ends. A hard edge is the two coinciding. Gauss-
     *  Legendre across a step converges as slowly as anything does; within a stretch it is exact to
     *  rounding for the polynomial part. */
        std::array<double, 6> cuts{};
        int count = 0;
        const auto cutAtColatitude = [&](double theta) {
            cuts[static_cast<std::size_t>(count++)] = std::cos(clampd(theta, 0.0, kPi));
        };
        const double half = std::max(own.softness, 0.0) * 0.5;
        cuts[static_cast<std::size_t>(count++)] = -1.0;
        cuts[static_cast<std::size_t>(count++)] = 1.0;
        if (own.kind == RegionKind::Spot) {
            cutAtColatitude(own.size - half);
            cutAtColatitude(own.size + half);
        } else {
            const double centre = kPi * 0.5 - own.bandElevation, reach = own.thickness * 0.5;
            cutAtColatitude(centre - reach - half);
            cutAtColatitude(centre - reach + half);
            cutAtColatitude(centre + reach - half);
            cutAtColatitude(centre + reach + half);
        }
        std::sort(cuts.begin(), cuts.begin() + count);

        const int N = order_;
        std::fill(blocks_.begin(), blocks_.end(), 0.0);
        for (int s = 0; s + 1 < count; ++s) {
            const double lo = cuts[static_cast<std::size_t>(s)], hi = cuts[static_cast<std::size_t>(s + 1)];
            if (hi - lo < 1e-14) continue;
            const double mid = 0.5 * (hi + lo), halfWidth = 0.5 * (hi - lo);
            if (valueAtHeight(mid) == 0.0 && valueAtHeight(lo + 0.25 * (hi - lo)) == 0.0 &&
                valueAtHeight(hi - 0.25 * (hi - lo)) == 0.0)
                continue;  // a stretch that is wholly outside adds nothing
            for (std::size_t k = 0; k < nodeZ_.size(); ++k) {
                const double z = mid + halfWidth * nodeZ_[k];
                const double g = valueAtHeight(z) * nodeW_[k] * halfWidth;
                if (g == 0.0) continue;
                shSN3D(Vec3{std::sqrt(std::max(0.0, 1.0 - z * z)), 0.0, z}, N, terms_);
                int at = 0;
                for (int m = 0; m <= N; ++m) {
                    const int w = N - m + 1;
                    for (int i = 0; i < w; ++i) {
                        const int ni = m + i;
                        const double left = g * terms_[static_cast<std::size_t>(ni * ni + ni + m)];
                        for (int j = 0; j < w; ++j) {
                            const int nj = m + j;
                            blocks_[static_cast<std::size_t>(at + w * i + j)] +=
                                left * terms_[static_cast<std::size_t>(nj * nj + nj + m)];
                        }
                    }
                    at += w * w;
                }
            }
        }
        //  Azimuth in closed form, and the (2n+1) of the column.
        int at = 0;
        for (int m = 0; m <= N; ++m) {
            const int w = N - m + 1;
            for (int i = 0; i < w; ++i)
                for (int j = 0; j < w; ++j)
                    blocks_[static_cast<std::size_t>(at + w * i + j)] *= (2.0 * (m + j) + 1.0) * (m == 0 ? 0.5 : 0.25);
            at += w * w;
        }
        axialBlocks(blocks_, N, axial_);
    }

    //  The turn that carries the world into the region's own frame, read off toOwnFrame itself so the
    //  two halves of regions cannot disagree about an orientation.
    const Vec3 ex = toOwnFrame(region, {1, 0, 0}), ey = toOwnFrame(region, {0, 1, 0}),
               ez = toOwnFrame(region, {0, 0, 1});
    const Mat3 r{ex.x, ey.x, ez.x, ex.y, ey.y, ez.y, ex.z, ey.z, ez.z};
    plain_ = r == kIdentity3;  // exactly: an upright kind at zero, where toOwnFrame turns nothing
    if (!plain_) {
        shRotation(r, order_, rotation_);
        fieldBlocks(rotation_, order_, toOwn_, fromOwn_);
    }
    return true;
}

void RegionOperator::apply(const float* in, float* out, int frames) noexcept {
    const std::size_t count = static_cast<std::size_t>(numChannels(order_) * frames);
    if (refused_ || identity_) {
        //  everything passes; the outside of everything is nothing, but a refused kind is not a region
        //  that was understood, so it passes whichever side was asked for
        if (identity_ && outside_)
            std::fill(out, out + count, 0.0f);
        else
            std::copy(in, in + count, out);
        return;
    }
    const float* cur = in;
    if (!plain_) {
        rotateField(toOwn_, order_, in, a_.data(), frames);
        cur = a_.data();
    }
    float* shaped = plain_ ? out : b_.data();
    if (axialKind_)
        applyAxial(axial_, {}, {}, order_, cur, shaped, frames);
    else {
        //  One dense multiply, frame outer, so a row's products accumulate in a register over a
        //  contiguous run of the input frame -- a shape clang turns into 4-wide fmla.
        const int C = numChannels(order_);
        for (int f = 0; f < frames; ++f) {
            const float* x = cur + f * C;
            float* y = shaped + f * C;
            for (int i = 0; i < C; ++i) {
                const float* row = denseF_.data() + static_cast<std::size_t>(i) * C;
                float sum = 0.0f;
                for (int c = 0; c < C; ++c) sum += row[c] * x[c];
                y[i] = sum;
            }
        }
    }
    if (!plain_) rotateField(fromOwn_, order_, shaped, out, frames);
    //  Outside is what inside left: I - M, exactly, rather than a second matrix that might not add up.
    if (outside_)
        for (std::size_t k = 0; k < count; ++k) out[k] = in[k] - out[k];
}

}  // namespace bambi
