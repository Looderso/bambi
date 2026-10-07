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

Vec3 directionOf(const Loudspeaker& s) {
    const double az = s.azimuthDeg * kDeg2Rad, el = s.elevationDeg * kDeg2Rad;
    return {std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
}

/// One hull face, ready to pan: gains for p are (p . n0, p . n1, p . n2) / det.
struct Face {
    std::array<int, 3> corner;
    Vec3 n0, n1, n2;
    double det;
};

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

void fibonacciSphere(int count, std::vector<Vec3>& out) {
    out.resize(sz(count));
    const double turn = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i) {
        const double k = i + 0.5, z = 1.0 - 2.0 * k / count, r = std::sqrt(std::max(0.0, 1.0 - z * z)), a = turn * k;
        out[sz(i)] = {r * std::cos(a), r * std::sin(a), z};
    }
}

bool designAllrad(const Layout& layout, int order, Weighting weighting, SpeakerDesign& out, std::string* error) {
    out = SpeakerDesign{};
    if (order < 1 || order > kMaxOrder) return fail(error, "order out of range");

    // The loudspeakers by channel, so that a layout listed in another order designs the same decoder.
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
        out.points.push_back(directionOf(s));
        out.imaginary.push_back(s.imaginary);
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

    if (ring) {
        // Virtual loudspeakers on the ring, panned between neighbours in azimuth.
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
            // The neighbours around phi, and the pair's gains: sines of the angles to the far neighbour.
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
        // Nothing below or above the listener: an imaginary point there, so a source there pans to the nearest ring.
        if (!below) {
            out.points.push_back({0.0, 0.0, -1.0});
            out.imaginary.push_back(true);
        }
        if (!above) {
            out.points.push_back({0.0, 0.0, 1.0});
            out.imaginary.push_back(true);
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
        std::vector<Face> faces;
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
            faces.push_back({corner, cross(b, c), cross(c, a), cross(a, b), dot(a, cross(b, c))});
            out.triangles.push_back(corner);
        }
        std::free(faceIndices);

        // Every grid direction panned to the triangle that holds it, with unit-energy gains.
        fibonacciSphere(kDesignGrid, grid);
        std::vector<double> d(sz(n) * sz(C), 0.0);
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
            const double norm = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            shSN3D(t, order, y);
            for (int i = 0; i < 3; ++i) {
                double* row = d.data() + sz(home->corner[sz(i)]) * sz(C);
                const double gain = g[i] / norm;
                for (int c = 0; c < C; ++c) row[c] += gain * y[sz(c)];
            }
        }
        for (int s = 0; s < n; ++s)
            for (int c = 0; c < C; ++c) d[sz(s) * sz(C) + sz(c)] *= col[sz(c)] / kDesignGrid;
        std::vector<Vec3> level;
        fibonacciSphere(kLevelGrid, level);
        normaliseEnergy(d, n, out.imaginary, order, level);
        out.matrix.swap(d);
    }

    // The real loudspeakers' rows on their channels; imaginary points and empty channels stay silent.
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
