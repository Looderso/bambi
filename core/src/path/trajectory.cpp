// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/path/trajectory.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sphere.hpp"
#include "bambi/path/generator.hpp"

namespace bambi {
namespace {

constexpr int kParametricSamplesPerLutPoint = 2;  ///< a shape is sampled twice as finely as it is tabulated
constexpr int kMinSegSamples = 24;
constexpr int kMaxSegSamples = 1024;
constexpr double kTargetStepRad = 0.15 * kDeg2Rad;

/// Cubic Bezier through four control points, projected back to the sphere.
Vec3 bezierAt(const Vec3 (&c)[4], double t) { return unit(cubicBezier(c, t)); }

/*  Samples per segment from the segment's own arc length, not a fixed count. A fixed count
 *  is uniform in Bezier parameter, which is not uniform in arc length — on a stretched
 *  handle the source spacing varied by three orders of magnitude, and the resampler below
 *  can only be as good as the polyline it reads.
 */
int segmentSamples(const Vec3 (&c)[4]) {
    double L = 0.0;
    Vec3 prev = bezierAt(c, 0.0);
    for (int k = 1; k <= 12; ++k) {
        const Vec3 q = bezierAt(c, k / 12.0);
        L += arc(prev, q);
        prev = q;
    }
    const int want = static_cast<int>(std::ceil(L / kTargetStepRad));
    return std::clamp(want, kMinSegSamples, kMaxSegSamples);
}

double polylineLength(const std::vector<PathSample>& pts, bool closed) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) total += arc(pts[i - 1].p, pts[i].p);
    if (closed && pts.size() > 1) total += arc(pts.back().p, pts.front().p);
    return total;
}

}  // namespace

int lutSizeFor(double lengthRad) {
    const double wanted = std::ceil(std::max(lengthRad, 0.0) / kMaxLutStepRad);
    return static_cast<int>(std::clamp(wanted, static_cast<double>(kMinLutSize), static_cast<double>(kMaxLutSize)));
}

std::vector<PathSample> samplePath(const TrajectoryState& s) {
    std::vector<PathSample> pts;
    samplePathInto(s, pts);
    return pts;
}

void PathScratch::reserveParametric() {
    const auto most = static_cast<std::size_t>(kMaxLutSize * kParametricSamplesPerLutPoint);
    samples.reserve(most);
    cumulative.reserve(most);
}

void samplePathInto(const TrajectoryState& s, std::vector<PathSample>& pts) {
    pts.clear();

    if (s.kind == TrajectoryKind::Parametric) {
        const bool closed = generatorIsClosed(s.generator);
        const auto sampleAt = [&](int m) {
            pts.clear();
            pts.reserve(static_cast<std::size_t>(m));
            for (int i = 0; i < m; ++i) {
                // An open path must reach u = 1; a closed one must not repeat u = 0.
                const double u = closed ? static_cast<double>(i) / m : static_cast<double>(i) / (m - 1);
                pts.push_back({generatorAt(s.generator, u, s.genParams), 0, u});
            }
        };
        //  At the density the shortest table needs first, then again as finely as its own table, only
        //  when that shows the path is too long for it.
        sampleAt(kMinLutSize * kParametricSamplesPerLutPoint);
        if (const int size = lutSizeFor(polylineLength(pts, closed)); size > kMinLutSize)
            sampleAt(size * kParametricSamplesPerLutPoint);
        return;
    }

    if (s.nodes.size() < 2) {
        if (s.nodes.size() == 1) pts.push_back({s.nodes[0].p, 0, 0.0});
        return;
    }

    const std::size_t nSeg = s.closed ? s.nodes.size() : s.nodes.size() - 1;
    for (std::size_t i = 0; i < nSeg; ++i) {
        Vec3 c[4];
        segmentControls(s.nodes, i, c);
        const int k = segmentSamples(c);
        for (int j = 0; j < k; ++j) {
            const double t = static_cast<double>(j) / k;
            pts.push_back({bezierAt(c, t), i, t});
        }
    }
    if (!s.closed) pts.push_back({s.nodes.back().p, nSeg - 1, 1.0});
}

void Trajectory::build(const TrajectoryState& s) {
    PathScratch scratch;
    build(s, scratch);
}

void Trajectory::build(const TrajectoryState& s, PathScratch& scratch) {
    lut_.clear();
    length_ = 0.0;
    closed_ = s.kind == TrajectoryKind::Parametric ? generatorIsClosed(s.generator) : s.closed;

    samplePathInto(s, scratch.samples);
    const std::vector<PathSample>& src = scratch.samples;
    const std::size_t n = src.size();
    if (n < 2) {
        if (n == 1) lut_.push_back(src[0].p);
        return;
    }

    std::vector<double>& cum = scratch.cumulative;
    cum.assign(n, 0.0);
    for (std::size_t i = 1; i < n; ++i) cum[i] = cum[i - 1] + arc(src[i - 1].p, src[i].p);
    const double total = closed_ ? cum[n - 1] + arc(src[n - 1].p, src[0].p) : cum[n - 1];
    if (total < 1e-9) {
        lut_.push_back(src[0].p);
        return;
    }

    const int size = lutSizeFor(total);
    lut_.resize(static_cast<std::size_t>(size));
    std::size_t j = 0;
    for (int i = 0; i < size; ++i) {
        const double target = total * (closed_ ? static_cast<double>(i) / size : static_cast<double>(i) / (size - 1));
        while (j + 1 < n && cum[j + 1] < target) ++j;
        const double hi = (j + 1 < n) ? cum[j + 1] : total;
        const double segLen = hi - cum[j];
        const double f = segLen > 1e-12 ? (target - cum[j]) / segLen : 0.0;
        lut_[static_cast<std::size_t>(i)] = unit(src[j].p + (src[(j + 1) % n].p - src[j].p) * f);
    }
    length_ = total;
}

Vec3 Trajectory::eval(double s) const {
    const std::size_t n = lut_.size();
    if (n == 0) return {1, 0, 0};
    if (n == 1) return lut_[0];

    if (closed_) {
        const double frac = s - std::floor(s);
        const double f = frac * static_cast<double>(n);
        const auto i = static_cast<std::size_t>(f);
        const double t = f - static_cast<double>(i);
        const Vec3& a = lut_[i % n];
        const Vec3& b = lut_[(i + 1) % n];
        return unit(a + (b - a) * t);
    }

    const double c = clampd(s, 0.0, 1.0);
    const double f = c * static_cast<double>(n - 1);
    const auto i = std::min(static_cast<std::size_t>(f), n - 2);
    const double t = f - static_cast<double>(i);
    return unit(lut_[i] + (lut_[i + 1] - lut_[i]) * t);
}

double Trajectory::phaseToS(double phase, MovementMode mode, bool closed) {
    if (closed || mode == MovementMode::Wrap) return phase - std::floor(phase);
    if (mode == MovementMode::PingPong) {
        const double h = phase * 0.5;
        const double f = (h - std::floor(h)) * 2.0;
        return f <= 1.0 ? f : 2.0 - f;
    }
    return clampd(phase, 0.0, 1.0);
}

Vec3 unapplyTransform(Vec3 p, Vec3 centre, const PathTransform& t) {
    //  Every step of applyTransform, undone in the opposite order.
    Vec3 v = p;
    if (t.yawRad != 0.0) v = rotateAxis(v, Vec3{0, 0, 1}, -t.yawRad);
    if (t.pitchRad != 0.0) v = rotateAxis(v, Vec3{0, 1, 0}, t.pitchRad);
    if (t.rollRad != 0.0) v = rotateAxis(v, centre, -t.rollRad);

    const double extent = clampd(t.extent, 0.0, kMaxExtent);
    if (std::abs(extent - 1.0) > 1e-6) {
        //  The slerp toward the centre, run backwards: the angle from the centre scaled up by 1/extent.
        //  Clamped at half a turn, which is as far as the sphere goes; at extent 0 there is nothing to
        //  undo, and the header says an editor must refuse before that (kEditableExtent).
        if (extent <= 1e-6) return centre;
        const double theta = arc(centre, v);
        if (theta <= 1e-12) return centre;
        return expMap(centre, unit(logMap(centre, v)) * clampd(theta / extent, 0.0, kPi));
    }
    return v;
}

Vec3 pathCentre(std::span<const Vec3> points) {
    if (points.empty()) return {0, 0, 1};

    /*  The discrete area vector: sum of p_i x p_{i+1}. For any planar loop this is normal to
     *  the plane with magnitude proportional to the enclosed area, which makes it correct
     *  for a great circle (where the mean is the origin) and for a small one alike. */
    Vec3 area{0, 0, 0}, mean{0, 0, 0};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3 a = points[i];
        const Vec3 b = points[(i + 1) % points.size()];
        area = area + cross(a, b);
        mean = mean + a;
    }

    const double areaLen = length(area);
    const double meanLen = length(mean);

    if (areaLen > 1e-9) {
        Vec3 n = area * (1.0 / areaLen);
        //  A path traversed the other way gives the opposite normal, which would shrink the
        //  source to the antipode. The mean says which side the path actually lives on.
        if (meanLen > 1e-6 && dot(n, mean) < 0.0) n = n * -1.0;
        return n;
    }
    if (meanLen > 1e-9) return mean * (1.0 / meanLen);
    return {0, 0, 1};
}

Vec3 applyTransform(Vec3 p, Vec3 centre, const PathTransform& t) {
    /*  Yaw and pitch rotate the path; they do not relocate its centre to an absolute
     *  direction. Read as absolute they would be the centroid's azimuth and elevation, which
     *  is only true of a path whose centre already sits at front -- and no generator's does.
     *  Their natural centres are orbit el -30, lissajous az 90, wave az 90 el 90, arc az -90,
     *  spiral el -84. Reading the parameters as absolute would make the default transform
     *  relocate every one of them, silently rewriting the shape the user authored. Identity at
     *  zero is the only defensible default, and it is also what the behaviour asks for:
     *  modulating yaw rotates the path around the listener. */
    Vec3 v = p;

    // Extent and roll act in the path's own frame, so they come first. Extent scales each point's angle
    // from the centre: under 1 it closes to a spot, over 1 it opens outward, and half a turn is the
    // antipode, where the sphere runs out and the point stays.
    const double extent = clampd(t.extent, 0.0, kMaxExtent);
    if (std::abs(extent - 1.0) > 1e-6) {
        const double theta = arc(centre, v);
        if (theta > 1e-12) v = expMap(centre, unit(logMap(centre, v)) * clampd(theta * extent, 0.0, kPi));
    }
    if (t.rollRad != 0.0) v = rotateAxis(v, centre, t.rollRad);

    // Then the world rotation. Pitch about +y (left) tilts front toward up; yaw about +z (up)
    // is CCW from front, which is the convention the whole project uses. Both wrap, so either
    // one can be driven by its rate and simply keep turning.
    if (t.pitchRad != 0.0) v = rotateAxis(v, Vec3{0, 1, 0}, -t.pitchRad);
    if (t.yawRad != 0.0) v = rotateAxis(v, Vec3{0, 0, 1}, t.yawRad);
    return v;
}

}  // namespace bambi
