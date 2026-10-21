// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/allrad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"

// The hull header declares its vertex as an anonymous struct inside a union, a GNU extension this build otherwise warns about.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "convhull_3d.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace bambi {

namespace {
constexpr double kSpeedOfSound = 343.0;
constexpr int kDesignGrid = 5200;    // directions panned to the loudspeakers
constexpr int kLevelGrid = 4000;     // a second grid, so the level is not judged on the points the design was fitted at
constexpr int kRingGrid = 360;       // one direction a degree, for a ring
constexpr double kRingDeg = 10.0;    // a layout is a ring when every point lies within this of the horizon
constexpr double kGainSlack = 1e-9;  // a panning gain this far below zero still counts as inside the triangle

std::size_t sz(int n) { return static_cast<std::size_t>(n); }

/// One hull face, ready to pan: gains for p are (p . n0, p . n1, p . n2) / det. A corner at or past `points.size()`
/// is the centre of a coplanar group of faces: its gain is shared equally by that polygon's corners.
struct Face {
    std::array<int, 3> corner;
    Vec3 n0, n1, n2;
    double det;
};

Face faceOf(const std::array<int, 3>& corner, const std::vector<Vec3>& at) {
    const Vec3 a = at[sz(corner[0])], b = at[sz(corner[1])], c = at[sz(corner[2])];
    return {corner, cross(b, c), cross(c, a), cross(a, b), dot(a, cross(b, c))};
}

/*  Four or more hull points on one plane -- a square of height loudspeakers -- leave the hull free to split them
 *  along either diagonal, and the panning would favour one. Each such group is split instead into a fan around its
 *  centre, a virtual point whose gain its corners share equally, so the panning is as even as the group.
 *  `faces` come in as the hull's triangles over `points`; they go out with the fans, and `points` grows by the
 *  centres (flagged imaginary and in `isCentre`) while `polygons` lists each centre's corners. */
void fanCoplanar(std::vector<std::array<int, 3>>& faces, std::vector<Vec3>& points, std::vector<bool>& imaginary,
                 std::vector<bool>& isCentre, std::vector<std::vector<int>>& polygons) {
    const int n = static_cast<int>(faces.size());
    std::vector<Vec3> normal(sz(n));
    std::vector<double> offset(sz(n));
    for (int f = 0; f < n; ++f) {
        const Vec3 a = points[sz(faces[sz(f)][0])], b = points[sz(faces[sz(f)][1])], c = points[sz(faces[sz(f)][2])];
        Vec3 nrm = cross(b - a, c - a);
        nrm = nrm * (1.0 / std::sqrt(dot(nrm, nrm)));
        normal[sz(f)] = nrm;
        offset[sz(f)] = dot(nrm, a);
    }
    std::vector<int> group(sz(n), -1);
    std::vector<std::vector<int>> members;
    for (int f = 0; f < n; ++f) {
        if (group[sz(f)] >= 0) continue;
        group[sz(f)] = static_cast<int>(members.size());
        members.push_back({f});
        for (int g = f + 1; g < n; ++g)
            if (group[sz(g)] < 0 && std::abs(dot(normal[sz(f)], normal[sz(g)]) - 1.0) < 1e-9 &&
                std::abs(offset[sz(f)] - offset[sz(g)]) < 1e-9) {
                group[sz(g)] = group[sz(f)];
                members.back().push_back(g);
            }
    }
    std::vector<std::array<int, 3>> out;
    for (const std::vector<int>& m : members) {
        if (m.size() == 1) {
            out.push_back(faces[sz(m[0])]);
            continue;
        }
        std::vector<int> corners;
        for (int f : m)
            for (int v : faces[sz(f)])
                if (std::find(corners.begin(), corners.end(), v) == corners.end()) corners.push_back(v);
        Vec3 centre{0, 0, 0};
        for (int v : corners) centre = centre + points[sz(v)];
        centre = centre * (1.0 / std::sqrt(dot(centre, centre)));
        //  The corners in order around the centre, in the plane.
        Vec3 u = points[sz(corners[0])] - centre * dot(points[sz(corners[0])], centre);
        u = u * (1.0 / std::sqrt(dot(u, u)));
        const Vec3 w = cross(centre, u);
        std::stable_sort(corners.begin(), corners.end(), [&](int a, int b) {
            return std::atan2(dot(points[sz(a)], w), dot(points[sz(a)], u)) <
                   std::atan2(dot(points[sz(b)], w), dot(points[sz(b)], u));
        });
        const int id = static_cast<int>(points.size());
        points.push_back(centre);
        imaginary.push_back(true);
        isCentre.push_back(true);
        polygons.push_back(corners);
        for (std::size_t k = 0; k < corners.size(); ++k)
            out.push_back({id, corners[k], corners[(k + 1) % corners.size()]});
    }
    faces.swap(out);
}

bool fail(std::string* error, const char* what) {
    if (error) *error = what;
    return false;
}

/// The SN3D input's weight per channel: (2n + 1) for a sampling decoder, times the order weight.
void columnWeights(int order, Weighting weighting, std::vector<double>& col) {
    std::vector<double> a(sz(order + 1), 1.0);
    if (weighting == Weighting::MaxRe) maxRE(order, a);
    col.resize(sz(numChannels(order)));
    for (int c = 0; c < numChannels(order); ++c) col[sz(c)] = (2.0 * acnOrder(c) + 1.0) * a[sz(acnOrder(c))];
}

/// D[s][c] /= sqrt(mean over the grid of the loudspeaker energy a plane wave gives), real loudspeakers only.
void normaliseEnergy(std::vector<double>& d, int speakers, const std::vector<bool>& imaginary, int order,
                     const std::vector<Vec3>& grid) {
    const int C = numChannels(order);
    std::vector<double> y(sz(C));
    double energy = 0;
    for (const Vec3& t : grid) {
        shSN3D(t, order, y);
        for (int s = 0; s < speakers; ++s) {
            if (imaginary[sz(s)]) continue;
            double g = 0;
            for (int c = 0; c < C; ++c) g += y[sz(c)] * d[sz(s) * sz(C) + sz(c)];
            energy += g * g;
        }
    }
    const double scale = 1.0 / std::sqrt(energy / static_cast<double>(grid.size()));
    for (double& v : d) v *= scale;
}
}  // namespace

/// The cardioid to the n-th power, (1 + x)^n / 2^n, as Legendre coefficients a_0..a_n: a_l = (2l + 1) / 2 times its
/// integral against P_l over -1..1, by Simpson's rule on a fine grid, exact enough for a polynomial of degree n.
std::vector<double> cardioidPowerCoefficients(int n) {
    const int steps = 4000;
    std::vector<double> a(sz(n + 1), 0.0);
    for (int l = 0; l <= n; ++l) {
        double sum = 0;
        for (int i = 0; i <= steps; ++i) {
            const double x = -1.0 + 2.0 * i / steps;
            const double w = (i == 0 || i == steps) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
            sum += w * std::pow((1.0 + x) / 2.0, n) * legendreP(l, x);
        }
        a[sz(l)] = (2.0 * l + 1.0) / 2.0 * sum * (2.0 / steps) / 3.0;
    }
    return a;
}

bool designAllrad(const Layout& layout, int order, Weighting weighting, SpeakerDesign& out, std::string* error,
                  const Microphones& microphones) {
    out = SpeakerDesign{};
    if (order < 1 || order > kMaxOrder) return fail(error, "order out of range");

    //  The loudspeakers by channel, so that a layout listed in another order designs the same decoder.
    std::vector<Loudspeaker> speakers = layout.speakers;
    std::stable_sort(speakers.begin(), speakers.end(),
                     [](const Loudspeaker& a, const Loudspeaker& b) { return a.channel < b.channel; });
    int realCount = 0;
    for (std::size_t i = 0; i < speakers.size(); ++i) {
        const Loudspeaker& s = speakers[i];
        if (s.channel < 1) return fail(error, "a channel below 1");
        if (i > 0 && speakers[i - 1].channel == s.channel) return fail(error, "two loudspeakers on one channel");
        if (!(s.radiusM > 0) || !std::isfinite(s.azimuthDeg) || !std::isfinite(s.elevationDeg))
            return fail(error, "a loudspeaker without a place");
        if (!s.imaginary) ++realCount;
    }
    if (realCount == 0) return fail(error, "no real loudspeaker");

    for (const Loudspeaker& s : speakers) {
        out.points.push_back(fromAzEl(s.azimuthDeg * kDeg2Rad, s.elevationDeg * kDeg2Rad));
        out.imaginary.push_back(s.imaginary);
        out.centre.push_back(false);
    }
    const double ringZ = std::sin(kRingDeg * kDeg2Rad);
    bool below = false, above = false, ring = true;
    for (const Vec3& p : out.points) {
        below = below || p.z < -ringZ;
        above = above || p.z > ringZ;
        ring = ring && std::abs(p.z) <= ringZ;
    }
    out.ring = ring;
    out.order = order;
    const int C = numChannels(order);
    std::vector<double> col;
    columnWeights(order, weighting, col);
    std::vector<double> y(sz(C));
    std::vector<Vec3> grid;

    if (realCount <= 2) {
        /*  One or two loudspeakers give the panning nothing to span, and are not meant to: they are the downmix a
            listener makes of the mix. Each real loudspeaker is a virtual microphone -- one loudspeaker an omni -- so
            nothing in the field vanishes and a source behind folds in in phase. A pair's microphones are the
            cardioid, or the cardioid to a power for a narrower pattern, pointed a quarter turn to either side of the
            pair's middle in the pair's own plane -- the classic stereo decode: a source on a loudspeaker's axis lands
            three to one on its side, one in the middle in both alike, and so does one behind -- or at the
            loudspeakers, where a narrow pattern hears the slice between them. The level is set as for every decoder:
            a plane wave gives, averaged over all directions, the energy of one loudspeaker. */
        out.ring = false;  // microphones, not a ring of virtual loudspeakers
        const int total = static_cast<int>(out.points.size());
        std::vector<double> d(sz(total) * sz(C), 0.0);
        const int pattern = std::clamp(microphones.pattern, 1, order);
        const std::vector<double> coefficient = cardioidPowerCoefficients(pattern);
        std::vector<double> basis(sz(numChannels(pattern)));
        Vec3 middle{0, 0, 0};
        for (int s = 0; s < total; ++s)
            if (!out.imaginary[sz(s)]) middle = middle + out.points[sz(s)];
        const bool apart = dot(middle, middle) > 1e-9;  // a pair facing each other has no middle: each points at itself
        for (int s = 0; s < total; ++s) {
            if (out.imaginary[sz(s)]) continue;
            d[sz(s) * sz(C)] = 1.0;  // the omni
            if (realCount == 2) {
                const Vec3 p = out.points[sz(s)];
                const Vec3 b = unit(middle);
                const Vec3 axis = microphones.atLoudspeakers || !apart ? p : unit(p - b * dot(p, b));
                shSN3D(axis, pattern, basis);
                for (int c = 0; c < numChannels(pattern); ++c)
                    d[sz(s) * sz(C) + sz(c)] = coefficient[sz(acnOrder(c))] * basis[sz(c)];
            }
        }
        std::vector<Vec3> level;
        fibonacciSphere(kLevelGrid, level);
        normaliseEnergy(d, total, out.imaginary, order, level);
        out.matrix.swap(d);
    } else if (ring) {
        //  Virtual loudspeakers on the ring, panned between neighbours in azimuth.
        const int n = static_cast<int>(out.points.size());
        std::vector<int> byAzimuth(sz(n));
        std::vector<double> az(sz(n));
        for (int i = 0; i < n; ++i) {
            byAzimuth[sz(i)] = i;
            az[sz(i)] = std::atan2(out.points[sz(i)].y, out.points[sz(i)].x);
            if (az[sz(i)] < 0) az[sz(i)] += 2.0 * kPi;
        }
        std::stable_sort(byAzimuth.begin(), byAzimuth.end(), [&](int a, int b) { return az[sz(a)] < az[sz(b)]; });
        for (int i = 0; i < n; ++i) {
            const double a = az[sz(byAzimuth[sz(i)])], b = az[sz(byAzimuth[sz((i + 1) % n)])];
            const double span = i + 1 < n ? b - a : b + 2.0 * kPi - a;
            if (!(span < kPi) || span <= 0) return fail(error, "the ring leaves a gap wider than a half turn");
        }
        std::vector<double> d(sz(n) * sz(C), 0.0);
        grid.resize(sz(kRingGrid));
        for (int k = 0; k < kRingGrid; ++k) {
            const double phi = 2.0 * kPi * k / kRingGrid;
            grid[sz(k)] = {std::cos(phi), std::sin(phi), 0.0};
            //  The neighbours around phi, and the pair's gains: sines of the angles to the far neighbour.
            int i = n - 1;
            for (int j = 0; j < n; ++j)
                if (az[sz(byAzimuth[sz(j)])] <= phi) i = j;
            const int s0 = byAzimuth[sz(i)], s1 = byAzimuth[sz((i + 1) % n)];
            const double a0 = az[sz(s0)], a1 = az[sz(s1)] + (i + 1 < n ? 0.0 : 2.0 * kPi);
            const double p = phi < a0 ? phi + 2.0 * kPi : phi;
            double g0 = std::sin(a1 - p) / std::sin(a1 - a0), g1 = std::sin(p - a0) / std::sin(a1 - a0);
            const double norm = std::sqrt(g0 * g0 + g1 * g1);
            g0 /= norm;
            g1 /= norm;
            shSN3D(grid[sz(k)], order, y);
            for (int c = 0; c < C; ++c) {
                d[sz(s0) * sz(C) + sz(c)] += g0 * y[sz(c)];
                d[sz(s1) * sz(C) + sz(c)] += g1 * y[sz(c)];
            }
        }
        for (int s = 0; s < n; ++s)
            for (int c = 0; c < C; ++c) d[sz(s) * sz(C) + sz(c)] *= col[sz(c)] / kRingGrid;
        normaliseEnergy(d, n, out.imaginary, order, grid);
        out.matrix.swap(d);
    } else {
        //  Nothing below or above the listener: an imaginary point there, so a source there pans to the nearest ring.
        if (!below) {
            out.points.push_back({0.0, 0.0, -1.0});
            out.imaginary.push_back(true);
            out.centre.push_back(false);
        }
        if (!above) {
            out.points.push_back({0.0, 0.0, 1.0});
            out.imaginary.push_back(true);
            out.centre.push_back(false);
        }
        const int n = static_cast<int>(out.points.size());
        if (n < 4) return fail(error, "too few points for a hull");

        std::vector<ch_vertex> vertices(sz(n));
        for (int i = 0; i < n; ++i)
            vertices[sz(i)] = {{{out.points[sz(i)].x, out.points[sz(i)].y, out.points[sz(i)].z}}};
        int* faceIndices = nullptr;
        int faceCount = 0;
        convhull_3d_build(vertices.data(), n, &faceIndices, &faceCount);
        if (faceIndices == nullptr || faceCount < 4) {
            std::free(faceIndices);
            return fail(error, "the loudspeakers do not span a hull");
        }
        Vec3 centroid{0, 0, 0};
        for (const Vec3& p : out.points) centroid = centroid + p * (1.0 / n);
        std::vector<std::array<int, 3>> triangles;
        for (int f = 0; f < faceCount; ++f) {
            std::array<int, 3> corner{faceIndices[3 * f], faceIndices[3 * f + 1], faceIndices[3 * f + 2]};
            const Vec3 a = out.points[sz(corner[0])];
            Vec3 b = out.points[sz(corner[1])], c = out.points[sz(corner[2])];
            Vec3 normal = cross(b - a, c - a);
            if (dot(normal, a - centroid) < 0) {  // wound the other way: turn it outward
                std::swap(corner[1], corner[2]);
                std::swap(b, c);
                normal = normal * -1.0;
            }
            const double len = std::sqrt(dot(normal, normal));
            if (!(len > 0) || dot(normal, a) / len <= 1e-9) {
                std::free(faceIndices);
                return fail(error, "the layout does not surround the listener");
            }
            triangles.push_back(corner);
        }
        std::free(faceIndices);
        std::vector<std::vector<int>> polygons;
        fanCoplanar(triangles, out.points, out.imaginary, out.centre, polygons);
        out.triangles = triangles;
        std::vector<Face> faces;
        faces.reserve(triangles.size());
        for (const std::array<int, 3>& t : triangles) faces.push_back(faceOf(t, out.points));
        const int total = static_cast<int>(out.points.size());

        //  Every grid direction panned to the triangle that holds it, a centre's gain shared by its polygon's
        //  corners, the gains at unit energy.
        fibonacciSphere(kDesignGrid, grid);
        std::vector<double> d(sz(total) * sz(C), 0.0), gains(sz(total));
        std::vector<int> touched;
        for (const Vec3& t : grid) {
            const Face* home = nullptr;
            double g[3] = {0, 0, 0};
            for (const Face& f : faces) {
                g[0] = dot(t, f.n0) / f.det;
                g[1] = dot(t, f.n1) / f.det;
                g[2] = dot(t, f.n2) / f.det;
                if (g[0] >= -kGainSlack && g[1] >= -kGainSlack && g[2] >= -kGainSlack) {
                    home = &f;
                    break;
                }
            }
            if (home == nullptr) return fail(error, "a direction fell outside every triangle");
            touched.clear();
            auto give = [&](int point, double gain) {
                if (gains[sz(point)] == 0.0) touched.push_back(point);
                gains[sz(point)] += gain;
            };
            for (int i = 0; i < 3; ++i) {
                const int corner = home->corner[sz(i)];
                if (corner < n)
                    give(corner, g[i]);
                else
                    for (int v : polygons[sz(corner - n)])
                        give(v, g[i] / static_cast<double>(polygons[sz(corner - n)].size()));
            }
            double energy = 0;
            for (int p : touched) energy += gains[sz(p)] * gains[sz(p)];
            const double norm = std::sqrt(energy);
            shSN3D(t, order, y);
            for (int p : touched) {
                double* row = d.data() + sz(p) * sz(C);
                const double gain = gains[sz(p)] / norm;
                for (int c = 0; c < C; ++c) row[c] += gain * y[sz(c)];
                gains[sz(p)] = 0.0;
            }
        }
        for (int s = 0; s < total; ++s)
            for (int c = 0; c < C; ++c) d[sz(s) * sz(C) + sz(c)] *= col[sz(c)] / kDesignGrid;
        std::vector<Vec3> level;
        fibonacciSphere(kLevelGrid, level);
        normaliseEnergy(d, total, out.imaginary, order, level);
        out.matrix.swap(d);
    }

    //  The real loudspeakers' rows on their channels; imaginary points and empty channels stay silent.
    std::vector<double> perPoint;
    perPoint.swap(out.matrix);
    double farthest = 0;
    for (const Loudspeaker& s : speakers)
        if (!s.imaginary) {
            out.outputs = std::max(out.outputs, s.channel);
            farthest = std::max(farthest, s.radiusM);
        }
    out.matrix.assign(sz(out.outputs) * sz(C), 0.0);
    for (std::size_t i = 0; i < speakers.size(); ++i) {
        const Loudspeaker& s = speakers[i];
        if (s.imaginary) continue;
        std::copy(perPoint.begin() + static_cast<std::ptrdiff_t>(i * sz(C)),
                  perPoint.begin() + static_cast<std::ptrdiff_t>((i + 1) * sz(C)),
                  out.matrix.begin() + static_cast<std::ptrdiff_t>(sz(s.channel - 1) * sz(C)));
        out.channel.push_back(s.channel - 1);
        out.gain.push_back(s.radiusM / farthest);
        out.delaySeconds.push_back((farthest - s.radiusM) / kSpeedOfSound);
    }
    return true;
}

}  // namespace bambi
