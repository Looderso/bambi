// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/scene/view.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {
namespace {

double distanceToSegment(double px, double py, const ScreenSegment& s) {
    const double vx = s.x1 - s.x0, vy = s.y1 - s.y0;
    const double len2 = vx * vx + vy * vy;
    const double t = len2 > 0.0 ? std::clamp(((px - s.x0) * vx + (py - s.y0) * vy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(px - (s.x0 + t * vx), py - (s.y0 + t * vy));
}

/*  The globe's inverse: a point in view space back onto the sphere. `z` chooses the side -- positive faces the
    viewer. The rotation is taken once, since a width's coverage asks it of every cell. */
struct GlobeInverse {
    double cp, sp, cy, sy;
    explicit GlobeInverse(const Camera& camera)
        : cp(std::cos(-camera.pitch)),
          sp(std::sin(-camera.pitch)),
          cy(std::cos(-camera.yaw)),
          sy(std::sin(-camera.yaw)) {}
    Vec3 at(double nx, double ny, double z) const {
        const double y1 = ny * cp - z * sp, z1 = ny * sp + z * cp;
        const double x2 = nx * cy + z1 * sy, z2 = -nx * sy + z1 * cy;
        return unit({z2, -x2, y1});
    }
};

}  // namespace

Camera cameraFor(ViewPreset preset) {
    switch (preset) {
        case ViewPreset::Front: return {0.0, 0.0};
        case ViewPreset::Side: return {kPi / 2.0, 0.0};  // the left side faces the viewer
        case ViewPreset::Top: return {0.0, kPi / 2.0};
        case ViewPreset::Free: break;
    }
    return {};
}

void orbit(Camera& camera, double dxPixels, double dyPixels) {
    camera.yaw += dxPixels * kOrbitRadPerPixel;
    camera.pitch = std::clamp(camera.pitch + dyPixels * kOrbitRadPerPixel, -kPi / 2.0, kPi / 2.0);
}

ViewPoint project(Projection kind, const Camera& camera, Vec3 p) {
    if (kind == Projection::Equirect) return {-azimuth(p) / kPi, elevation(p) / (kPi / 2.0), 1.0};

    //  Screen right is the listener's right (-y), up is up (z), and the front (x) faces the viewer;
    //  then yaw about the vertical and pitch about the horizontal.
    const double x = -p.y, y = p.z, z = p.x;
    const double cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    const double cp = std::cos(camera.pitch), sp = std::sin(camera.pitch);
    const double x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
    const double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
    return {x1, y2, z2};
}

std::optional<Vec3> unproject(Projection kind, const Camera& camera, double nx, double ny) {
    if (kind == Projection::Equirect) {
        if (std::abs(nx) > 1.0 || std::abs(ny) > 1.0) return std::nullopt;
        return fromAzEl(-nx * kPi, ny * kPi / 2.0);
    }
    const double r2 = nx * nx + ny * ny;
    if (r2 > 1.0) return std::nullopt;
    return GlobeInverse(camera).at(nx, ny, std::sqrt(1.0 - r2));  // the visible side
}

Vec3 unprojectToRim(Projection kind, const Camera& camera, double nx, double ny) {
    if (kind == Projection::Equirect)
        return fromAzEl(-std::clamp(nx, -1.0, 1.0) * kPi, std::clamp(ny, -1.0, 1.0) * kPi / 2.0);
    const double r2 = nx * nx + ny * ny;
    if (r2 <= 1.0) return GlobeInverse(camera).at(nx, ny, std::sqrt(1.0 - r2));
    /*  Off the disc: the rim in the pointer's direction -- a hair on the visible side of it. Exactly on
        the rim a point is neither in front nor behind, and rounding flipped a dragged handle between
        the two every frame. */
    constexpr double kInside = 0.9999;
    const double s = kInside / std::sqrt(r2);
    return GlobeInverse(camera).at(nx * s, ny * s, std::sqrt(1.0 - kInside * kInside));
}

ScreenPoint toScreen(Projection kind, const Camera& camera, const Viewport& vp, Vec3 p) {
    const ViewPoint n = project(kind, camera, p);
    return {vp.screenX(n.x), vp.screenY(n.y), n.depth};
}

void capCoverage(Projection kind, const Camera& camera, const Viewport& vp, Vec3 centre, double radiusRad,
                 int cellPixels, CapCoverage& out) {
    out.cell = std::max(1, cellPixels);
    out.cols = out.rows = 0;
    out.front.clear();
    out.back.clear();
    if (!(radiusRad > 0.0)) return;
    const bool globe = kind == Projection::Globe;
    const Vec3 c = unit(centre);
    const double radius = std::min(radiusRad, kPi);

    //  The part of normalised space to sample. The projected boundary bounds the cap, unless the cap wraps:
    //  past a hemisphere, round the globe's edge, or over a pole. A cap across the equirect's back seam needs
    //  no case of its own -- its boundary lands at both edges, so its bounds already span the frame.
    double nx0 = -1.0, nx1 = 1.0, ny0 = -1.0, ny1 = 1.0;
    if (radius < kPi / 2.0) {
        const Vec3 helper = std::abs(c.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
        const Vec3 u = unit(cross(c, helper)), v = cross(c, u);
        const double cr = std::cos(radius), sr = std::sin(radius);
        constexpr int kSamples = 72;
        double minX = 1.0, maxX = -1.0, minY = 1.0, maxY = -1.0;
        bool front = false, behind = false;
        for (int i = 0; i < kSamples; ++i) {
            const double t = 2.0 * kPi * i / kSamples, ct = std::cos(t), st = std::sin(t);
            const Vec3 q{c.x * cr + (u.x * ct + v.x * st) * sr, c.y * cr + (u.y * ct + v.y * st) * sr,
                         c.z * cr + (u.z * ct + v.z * st) * sr};
            const ViewPoint p = project(kind, camera, q);
            minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
            (p.depth > 0.0 ? front : behind) = true;
        }
        const bool pole = !globe && (arc(c, {0.0, 0.0, 1.0}) < radius || arc(c, {0.0, 0.0, -1.0}) < radius);
        const bool whole = globe ? (front && behind) : pole;
        if (!whole) nx0 = minX, nx1 = maxX, ny0 = minY, ny1 = maxY;
    }

    //  In pixels, padded by a cell so the softened edge is not cut off, and kept inside the frame.
    const double cell = out.cell;
    const double px0 = std::max(vp.screenX(-1.0), vp.screenX(nx0) - cell),
                 px1 = std::min(vp.screenX(1.0), vp.screenX(nx1) + cell);
    const double py0 = std::max(vp.screenY(1.0), vp.screenY(ny1) - cell),
                 py1 = std::min(vp.screenY(-1.0), vp.screenY(ny0) + cell);
    if (!(px1 > px0) || !(py1 > py0)) return;
    out.x0 = static_cast<int>(std::floor(px0));
    out.y0 = static_cast<int>(std::floor(py0));
    out.cols = static_cast<int>(std::ceil((px1 - out.x0) / cell));
    out.rows = static_cast<int>(std::ceil((py1 - out.y0) / cell));
    const auto cells = static_cast<std::size_t>(out.cols) * static_cast<std::size_t>(out.rows);
    out.front.assign(cells, 0.0f);
    if (globe) out.back.assign(cells, 0.0f);

    //  One cell's angle, at the view's centre: the edge is softened over it.
    const double soft = globe ? cell / vp.rx : cell * kPi / vp.rx;
    const bool everywhere = radius >= kPi - 1e-9;
    const auto cover = [&](Vec3 p) {
        return everywhere ? 1.0f : static_cast<float>(std::clamp((radius - arc(c, p)) / soft + 0.5, 0.0, 1.0));
    };
    const GlobeInverse inverse(camera);
    for (int row = 0; row < out.rows; ++row) {
        const double ny = -(out.y0 + (row + 0.5) * cell - vp.cy) / vp.ry;
        for (int col = 0; col < out.cols; ++col) {
            const double nx = (out.x0 + (col + 0.5) * cell - vp.cx) / vp.rx;
            const auto k =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(out.cols) + static_cast<std::size_t>(col);
            if (globe) {
                const double r2 = nx * nx + ny * ny;
                if (r2 > 1.0) continue;
                const double z = std::sqrt(1.0 - r2);
                out.front[k] = cover(inverse.at(nx, ny, z));
                out.back[k] = cover(inverse.at(nx, ny, -z));
            } else {
                if (std::abs(nx) > 1.0 || std::abs(ny) > 1.0) continue;
                out.front[k] = cover(fromAzEl(-nx * kPi, ny * kPi / 2.0));
            }
        }
    }
}

void projectPolyline(Projection kind, const Camera& camera, const Viewport& vp, std::span<const Vec3> points,
                     bool closed, std::vector<ScreenSegment>& out) {
    const std::size_t n = points.size();
    if (n < 2) return;
    const std::size_t last = closed ? n : n - 1;
    ViewPoint a = project(kind, camera, points[0]);
    for (std::size_t i = 0; i < last; ++i) {
        const ViewPoint b = project(kind, camera, points[(i + 1) % n]);
        const bool front = a.depth + b.depth > 0.0;
        const double dx = b.x - a.x;
        if (kind == Projection::Equirect && std::abs(dx) > 1.0) {
            //  The short way round crosses the back seam: end at one edge, resume at the other.
            const double bx = dx > 0.0 ? b.x - 2.0 : b.x + 2.0;
            const double edge = dx > 0.0 ? -1.0 : 1.0;
            const double t = (edge - a.x) / (bx - a.x);
            const double ym = a.y + (b.y - a.y) * t;
            out.push_back({vp.screenX(a.x), vp.screenY(a.y), vp.screenX(edge), vp.screenY(ym), front});
            out.push_back({vp.screenX(-edge), vp.screenY(ym), vp.screenX(b.x), vp.screenY(b.y), front});
        } else {
            out.push_back({vp.screenX(a.x), vp.screenY(a.y), vp.screenX(b.x), vp.screenY(b.y), front});
        }
        a = b;
    }
}

const std::vector<GraticuleLine>& graticule() {
    static const std::vector<GraticuleLine> lines = [] {
        std::vector<GraticuleLine> out;
        for (int el = -60; el <= 60; el += 30) {
            GraticuleLine line;
            line.major = el == 0;
            for (int i = 0; i <= 72; ++i) line.points.push_back(fromAzEl((-180.0 + i * 5.0) * kDeg2Rad, el * kDeg2Rad));
            out.push_back(std::move(line));
        }
        for (int az = 0; az < 360; az += 30) {
            GraticuleLine line;
            line.major = az == 0;
            for (int i = 0; i <= 24; ++i) line.points.push_back(fromAzEl(az * kDeg2Rad, (-90.0 + i * 7.5) * kDeg2Rad));
            out.push_back(std::move(line));
        }
        return out;
    }();
    return lines;
}

void transformedNodes(const TrajectoryState& s, Vec3 centre, const PathTransform& t, std::vector<Vec3>& out) {
    out.clear();
    out.reserve(s.nodes.size());
    for (const Node& n : s.nodes) out.push_back(applyTransform(n.p, centre, t));
}

int pickNode(Projection kind, const Camera& camera, const Viewport& vp, std::span<const Vec3> nodes, double x,
             double y) {
    int best = -1;
    double nearest = kNodeHitPixels;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const ScreenPoint s = toScreen(kind, camera, vp, nodes[i]);
        if (kind == Projection::Globe && s.depth <= 0.0) continue;  // you hit what you see
        const double d = std::hypot(s.x - x, s.y - y);
        if (d < nearest) {
            nearest = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

HandleHit pickHandle(Projection kind, const Camera& camera, const Viewport& vp, Vec3 in, bool inLive, Vec3 out,
                     bool outLive, double x, double y) {
    HandleHit hit;
    double nearest = kHandleHitPixels;
    const auto consider = [&](Vec3 p, bool live, Handle which) {
        if (!live) return;
        const ScreenPoint s = toScreen(kind, camera, vp, p);
        if (kind == Projection::Globe && s.depth <= 0.0) return;
        const double d = std::hypot(s.x - x, s.y - y);
        if (d < nearest) {
            nearest = d;
            hit = {true, which};
        }
    };
    consider(in, inLive, Handle::In);
    consider(out, outLive, Handle::Out);
    return hit;
}

CurveHit nearestOnCurveScreen(Projection kind, const Camera& camera, const Viewport& vp, const TrajectoryState& s,
                              Vec3 centre, const PathTransform& t, double x, double y) {
    CurveHit hit;
    const std::vector<PathSample> samples = samplePath(s);  // a click, not a frame: allocating here is fine
    if (samples.size() < 2) return hit;

    double nearest = kInsertHitPixels;
    const auto at = [&](const PathSample& sample) {
        return toScreen(kind, camera, vp, applyTransform(sample.p, centre, t));
    };
    const std::size_t last = s.closed ? samples.size() : samples.size() - 1;
    for (std::size_t i = 0; i < last; ++i) {
        const PathSample& a = samples[i];
        const PathSample& b = samples[(i + 1) % samples.size()];
        const ScreenPoint p = at(a), q = at(b);
        if (kind == Projection::Globe && (p.depth <= 0.0 || q.depth <= 0.0)) continue;
        //  A pair that crosses the equirect's back seam is drawn as two pieces at opposite edges; measuring
        //  the straight line between them would be a line across the whole panel.
        if (kind == Projection::Equirect && std::abs(p.x - q.x) > vp.rx) continue;

        const ScreenSegment segment{p.x, p.y, q.x, q.y, true};
        const double d = distanceToSegment(x, y, segment);
        if (d >= nearest) continue;
        nearest = d;

        //  How far along the pair the cursor fell, carried back to the Bezier parameter. Across a segment
        //  boundary the two samples have no common parameter, so the nearer end wins.
        const double vx = q.x - p.x, vy = q.y - p.y;
        const double len2 = vx * vx + vy * vy;
        const double u = len2 > 0.0 ? clampd(((x - p.x) * vx + (y - p.y) * vy) / len2, 0.0, 1.0) : 0.0;
        hit.found = true;
        hit.distancePixels = d;
        if (a.segment == b.segment) {
            hit.segment = a.segment;
            hit.t = a.t + (b.t - a.t) * u;
        } else if (u < 0.5) {
            hit.segment = a.segment;
            hit.t = a.t;
        } else {
            hit.segment = b.segment;
            hit.t = b.t;
        }
        hit.position = unit(applyTransform(a.p, centre, t) * (1.0 - u) + applyTransform(b.p, centre, t) * u);
    }
    return hit;
}

int pick(Projection kind, const Camera& camera, const Viewport& vp, std::span<const SceneTarget> targets, double x,
         double y) {
    int best = -1;
    double nearest = kDotHitPixels;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const ScreenPoint s = toScreen(kind, camera, vp, targets[i].position);
        if (kind == Projection::Globe && s.depth <= 0.0) continue;
        const double d = std::hypot(s.x - x, s.y - y);
        if (d < nearest) {
            nearest = d;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) return best;

    nearest = kPathHitPixels;
    std::vector<ScreenSegment> segments;  // a click, not a frame: allocating here is fine
    for (std::size_t i = 0; i < targets.size(); ++i) {
        segments.clear();
        projectPolyline(kind, camera, vp, targets[i].path, targets[i].closed, segments);
        for (const auto& segment : segments) {
            if (!segment.front) continue;
            const double d = distanceToSegment(x, y, segment);
            if (d < nearest) {
                nearest = d;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

}  // namespace bambi
