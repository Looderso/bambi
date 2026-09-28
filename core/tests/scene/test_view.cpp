// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The scene's geometry: projections, presets, the graticule, hit-testing and the click-or-drag rule.

#include <cmath>
#include <random>
#include <vector>

#include "bambi/link/link.hpp"
#include "bambi/path/authoring.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"
#include "bambi/scene/view.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("the listener's left is screen left, in the globe and in the equirect") {
    const Camera front = cameraFor(ViewPreset::Front);
    const ViewPoint f = project(Projection::Globe, front, {1.0, 0.0, 0.0});
    CHECK(f.x == doctest::Approx(0.0));
    CHECK(f.y == doctest::Approx(0.0));
    CHECK(f.depth > 0.0);
    CHECK(project(Projection::Globe, front, {0.0, 1.0, 0.0}).x == doctest::Approx(-1.0));
    CHECK(project(Projection::Globe, front, {0.0, 0.0, 1.0}).y == doctest::Approx(1.0));

    //  back, left, front, right, back -- left to right
    CHECK(project(Projection::Equirect, {}, {0.0, 1.0, 0.0}).x == doctest::Approx(-0.5));
    CHECK(project(Projection::Equirect, {}, {1.0, 0.0, 0.0}).x == doctest::Approx(0.0));
    CHECK(project(Projection::Equirect, {}, {0.0, -1.0, 0.0}).x == doctest::Approx(0.5));
    CHECK(std::abs(project(Projection::Equirect, {}, {-1.0, 0.0, 0.0}).x) == doctest::Approx(1.0));
    CHECK(project(Projection::Equirect, {}, {0.0, 0.0, 1.0}).y == doctest::Approx(1.0));
}

TEST_CASE("the presets look from the front, from the left side, and from above") {
    const Camera side = cameraFor(ViewPreset::Side);
    const ViewPoint left = project(Projection::Globe, side, {0.0, 1.0, 0.0});
    CHECK(left.x == doctest::Approx(0.0));
    CHECK(left.depth > 0.0);

    const Camera top = cameraFor(ViewPreset::Top);
    const ViewPoint up = project(Projection::Globe, top, {0.0, 0.0, 1.0});
    CHECK(up.x == doctest::Approx(0.0));
    CHECK(up.y == doctest::Approx(0.0));
    CHECK(up.depth == doctest::Approx(1.0));
    CHECK(project(Projection::Globe, top, {0.0, 1.0, 0.0}).x == doctest::Approx(-1.0));  // left stays left
    CHECK(project(Projection::Globe, top, {1.0, 0.0, 0.0}).y == doctest::Approx(-1.0));  // so front is down

    //  Orthographic from above: a source's distance from the centre is cos(elevation).
    const ViewPoint high = project(Projection::Globe, top, fromAzEl(0.7, 60.0 * kDeg2Rad));
    CHECK(std::hypot(high.x, high.y) == doctest::Approx(std::cos(60.0 * kDeg2Rad)));
}

TEST_CASE("picking a point back off the view is the inverse of projecting it, on the visible side") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p = unit({u(rng), u(rng), u(rng)});
        const Camera camera{u(rng) * kPi, u(rng) * kPi / 2.0};
        const ViewPoint n = project(Projection::Globe, camera, p);
        if (n.depth < 1e-3) continue;  // the far side is not pickable, by design
        const auto back = unproject(Projection::Globe, camera, n.x, n.y);
        REQUIRE(back.has_value());
        CHECK(sameDir(*back, p, 1e-9));
        ++checked;
    }
    CHECK(checked > 800);

    for (int i = 0; i < 500; ++i) {
        const Vec3 p = fromAzEl(u(rng) * 0.99 * kPi, u(rng) * 0.99 * kPi / 2.0);
        const ViewPoint n = project(Projection::Equirect, {}, p);
        const auto back = unproject(Projection::Equirect, {}, n.x, n.y);
        REQUIRE(back.has_value());
        CHECK(sameDir(*back, p, 1e-9));
    }
    CHECK_FALSE(unproject(Projection::Globe, {}, 0.8, 0.8).has_value());      // outside the disc
    CHECK_FALSE(unproject(Projection::Equirect, {}, 1.01, 0.0).has_value());  // outside the frame
}

TEST_CASE("orbiting turns at a fixed rate per pixel and stops at the poles") {
    Camera c = cameraFor(ViewPreset::Front);
    orbit(c, 100.0, 0.0);
    CHECK(c.yaw == doctest::Approx(0.8));
    orbit(c, 0.0, 10000.0);
    CHECK(c.pitch == doctest::Approx(kPi / 2.0));
    orbit(c, 0.0, -30000.0);
    CHECK(c.pitch == doctest::Approx(-kPi / 2.0));
}

TEST_CASE("a path across the equirect's back seam is split at the edges, never drawn across the frame") {
    std::vector<Vec3> equator;
    for (int i = 0; i < 128; ++i) equator.push_back(fromAzEl(2.0 * kPi * i / 128.0, 0.0));
    const Viewport vp{150.0, 100.0, 125.0, 100.0};
    std::vector<ScreenSegment> segments;
    projectPolyline(Projection::Equirect, {}, vp, equator, true, segments);

    double widest = 0.0;
    bool leftEdge = false, rightEdge = false;
    for (const auto& s : segments) {
        widest = std::max(widest, std::abs(s.x1 - s.x0));
        leftEdge = leftEdge || std::abs(std::min(s.x0, s.x1) - vp.screenX(-1.0)) < 1e-9;
        rightEdge = rightEdge || std::abs(std::max(s.x0, s.x1) - vp.screenX(1.0)) < 1e-9;
    }
    CHECK(widest < 0.05 * vp.rx);  // the seam segment was split, not drawn 250 px wide
    CHECK(leftEdge);
    CHECK(rightEdge);
    CHECK(segments.size() == equator.size() + 1);
}

TEST_CASE("the globe marks the back hemisphere, so it can be drawn fainter") {
    std::vector<Vec3> equator;
    for (int i = 0; i < 360; ++i) equator.push_back(fromAzEl(2.0 * kPi * i / 360.0, 0.0));
    std::vector<ScreenSegment> segments;
    projectPolyline(Projection::Globe, cameraFor(ViewPreset::Front), {100.0, 100.0, 90.0, 90.0}, equator, true,
                    segments);
    int front = 0;
    for (const auto& s : segments) front += s.front ? 1 : 0;
    CHECK(front > 170);
    CHECK(front < 190);  // half of it faces away from a front view
}

TEST_CASE("the graticule: five latitudes and twelve meridians, one major line of each") {
    const auto& lines = graticule();
    CHECK(lines.size() == 17);
    int major = 0;
    for (const auto& l : lines) major += l.major ? 1 : 0;
    CHECK(major == 2);
    CHECK(&graticule() == &lines);  // built once
}

TEST_CASE("picking: a dot wins over a path, a path selects its instance, and only what faces you is hit") {
    const Camera front = cameraFor(ViewPreset::Front);
    const Viewport vp{100.0, 100.0, 100.0, 100.0};

    std::vector<Vec3> arcAbove;  // high in front, from 60 degrees left to 60 degrees right
    for (int i = 0; i <= 24; ++i) arcAbove.push_back(fromAzEl((-60.0 + 5.0 * i) * kDeg2Rad, 45.0 * kDeg2Rad));

    std::vector<SceneTarget> targets(2);
    targets[0].position = {1.0, 0.0, 0.0};   // dead centre of the view
    targets[1].position = {-1.0, 0.0, 0.0};  // behind: projects to the centre too
    targets[1].path = arcAbove;
    targets[1].closed = false;

    CHECK(pick(Projection::Globe, front, vp, targets, 105.0, 100.0) == 0);  // the front dot, not the back one

    std::vector<SceneTarget> onlyBehind(targets.begin() + 1, targets.end());
    onlyBehind[0].path = {};
    CHECK(pick(Projection::Globe, front, vp, onlyBehind, 100.0, 100.0) == -1);  // hidden: not hittable

    const ScreenPoint onArc = toScreen(Projection::Globe, front, vp, arcAbove[12]);
    CHECK(pick(Projection::Globe, front, vp, targets, onArc.x + 6.0, onArc.y) == 1);  // its path selects it
    CHECK(pick(Projection::Globe, front, vp, targets, 10.0, 190.0) == -1);            // nothing there

    //  A path wholly behind the listener projects across the view, but cannot be clicked.
    std::vector<Vec3> arcBehind;
    for (int i = 0; i <= 24; ++i) arcBehind.push_back(fromAzEl((120.0 + 5.0 * i) * kDeg2Rad, 10.0 * kDeg2Rad));
    std::vector<SceneTarget> hiddenPath(1);
    hiddenPath[0].position = {-1.0, 0.0, 0.0};
    hiddenPath[0].path = arcBehind;
    hiddenPath[0].closed = false;
    const ScreenPoint behind = toScreen(Projection::Globe, front, vp, arcBehind[12]);
    CHECK(pick(Projection::Globe, front, vp, hiddenPath, behind.x, behind.y) == -1);

    //  A dot within reach beats a path that is closer.
    std::vector<SceneTarget> crowded(2);
    crowded[0].position = unit({1.0, 0.0, 0.1});
    crowded[1].position = {-1.0, 0.0, 0.0};
    std::vector<Vec3> throughCentre{fromAzEl(-0.3, 0.0), fromAzEl(0.3, 0.0)};
    crowded[1].path = throughCentre;
    crowded[1].closed = false;
    const ScreenPoint dot = toScreen(Projection::Globe, front, vp, crowded[0].position);
    CHECK(pick(Projection::Globe, front, vp, crowded, dot.x, 100.0) == 0);

    //  The equirect has no hidden side. Straight behind is azimuth 180, the left edge of the frame.
    CHECK(pick(Projection::Equirect, {}, {150.0, 100.0, 125.0, 100.0}, onlyBehind, 150.0 - 125.0, 100.0) == 0);
}

TEST_CASE("a published path is drawn with its transform applied about its centre") {
    LinkStatic st;
    st.pointCount = 3;
    st.path[0] = {1.0f, 0.0f, 0.0f};
    st.path[1] = {0.0f, 1.0f, 0.0f};
    st.path[2] = {0.0f, 0.0f, 1.0f};
    st.centre = {0.0f, 0.0f, 1.0f};

    LinkDynamic still;
    std::vector<Vec3> drawn;
    resolvePath(st, still, drawn);
    REQUIRE(drawn.size() == 3);
    for (int i = 0; i < 3; ++i) CHECK(sameDir(drawn[static_cast<std::size_t>(i)], st.point(i), 1e-6));

    LinkDynamic moved;
    moved.yawRad = 0.5f;
    moved.rollRad = 0.25f;
    moved.extent = 0.6f;
    resolvePath(st, moved, drawn);
    REQUIRE(drawn.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(sameDir(drawn[static_cast<std::size_t>(i)],
                      applyTransform(st.point(i), st.centreVec(), moved.transform()), 1e-12));
}

// ------------------------------------------------------------------ editing a chain in the scene

namespace {

TrajectoryState customChain(bool closed = true) {
    TrajectoryState s;
    s.generator = GeneratorType::Orbit;
    generatorDefaults(s.generator, s.genParams);
    convertToCustom(s, 8);
    setClosed(s, closed);
    return s;
}

Viewport testViewport() {
    Viewport vp;
    vp.cx = 150.0;
    vp.cy = 150.0;
    vp.rx = vp.ry = 120.0;
    return vp;
}

}  // namespace

TEST_CASE("a node is picked where it is drawn, and only on the side you can see") {
    const TrajectoryState s = customChain();
    const Camera camera = cameraFor(ViewPreset::Front);
    const Viewport vp = testViewport();
    std::vector<Vec3> drawn;
    transformedNodes(s, {0.0, 0.0, 1.0}, {}, drawn);
    REQUIRE(drawn.size() == s.nodes.size());

    int front = -1, back = -1;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const ScreenPoint p = toScreen(Projection::Globe, camera, vp, drawn[i]);
        (p.depth > 0.0 ? front : back) = static_cast<int>(i);
    }
    REQUIRE(front >= 0);
    REQUIRE(back >= 0);

    const ScreenPoint hit = toScreen(Projection::Globe, camera, vp, drawn[static_cast<std::size_t>(front)]);
    CHECK(pickNode(Projection::Globe, camera, vp, drawn, hit.x, hit.y) == front);
    CHECK(pickNode(Projection::Globe, camera, vp, drawn, hit.x + kNodeHitPixels + 2.0, hit.y) != front);

    const ScreenPoint hidden = toScreen(Projection::Globe, camera, vp, drawn[static_cast<std::size_t>(back)]);
    CHECK(pickNode(Projection::Globe, camera, vp, drawn, hidden.x, hidden.y) != back);

    //  The equirect hides nothing: every node is reachable there.
    const ScreenPoint flat = toScreen(Projection::Equirect, camera, vp, drawn[static_cast<std::size_t>(back)]);
    CHECK(pickNode(Projection::Equirect, camera, vp, drawn, flat.x, flat.y) == back);
}

TEST_CASE("a handle is picked only while it takes part in the curve") {
    const TrajectoryState open = customChain(false);
    const Camera camera = cameraFor(ViewPreset::Front);
    const Viewport vp = testViewport();

    const std::size_t middle = open.nodes.size() / 2;
    const Node& n = open.nodes[middle];
    const ScreenPoint out = toScreen(Projection::Equirect, camera, vp, n.cout);
    const HandleHit found = pickHandle(Projection::Equirect, camera, vp, n.cin, handleIsLive(open, middle, Handle::In),
                                       n.cout, handleIsLive(open, middle, Handle::Out), out.x, out.y);
    CHECK(found.found);
    CHECK(found.which == Handle::Out);

    //  An open path's first node has no live in-handle: it belongs to a derived endpoint, and dragging it
    //  would be a lie (authoring.hpp).
    const Node& first = open.nodes.front();
    REQUIRE_FALSE(handleIsLive(open, 0, Handle::In));
    const ScreenPoint inert = toScreen(Projection::Equirect, camera, vp, first.cin);
    CHECK_FALSE(pickHandle(Projection::Equirect, camera, vp, first.cin, handleIsLive(open, 0, Handle::In), first.cout,
                           handleIsLive(open, 0, Handle::Out), inert.x, inert.y)
                    .found);
}

TEST_CASE("a click near the curve reports the segment and t an insert would land on") {
    TrajectoryState s = customChain();
    const Camera camera = cameraFor(ViewPreset::Front);
    const Viewport vp = testViewport();
    const Vec3 centre{0.0, 0.0, 1.0};
    const PathTransform none;

    //  Aim at a sample in the middle of a segment, on the visible side.
    const auto samples = samplePath(s);
    const PathSample* target = nullptr;
    for (const auto& sample : samples) {
        if (sample.t < 0.4 || sample.t > 0.6) continue;
        if (toScreen(Projection::Globe, camera, vp, sample.p).depth > 0.0) {
            target = &sample;
            break;
        }
    }
    REQUIRE(target != nullptr);
    const ScreenPoint at = toScreen(Projection::Globe, camera, vp, target->p);

    const CurveHit hit = nearestOnCurveScreen(Projection::Globe, camera, vp, s, centre, none, at.x, at.y);
    REQUIRE(hit.found);
    CHECK(hit.distancePixels < 1.0);
    CHECK(hit.segment == target->segment);
    CHECK(hit.t == doctest::Approx(target->t).epsilon(0.05));
    CHECK(arc(hit.position, target->p) < 0.02);

    //  What the editor then does: the node lands where the click was, and nothing else moves.
    const std::size_t before = s.nodes.size();
    const std::size_t inserted = insertNode(s, hit.segment, clampd(hit.t, kInsertTMin, kInsertTMax));
    REQUIRE(s.nodes.size() == before + 1);
    CHECK(arc(s.nodes[inserted].p, target->p) < 0.02);

    //  Far from the curve, nothing is offered.
    CHECK_FALSE(nearestOnCurveScreen(Projection::Globe, camera, vp, s, centre, none, vp.cx, vp.cy).found);
}

TEST_CASE("nodes are picked where the transform draws them, not where they are authored") {
    const TrajectoryState s = customChain();
    const Camera camera = cameraFor(ViewPreset::Front);
    const Viewport vp = testViewport();
    std::vector<Vec3> dense;
    for (const auto& x : samplePath(s)) dense.push_back(x.p);
    const Vec3 centre = pathCentre(dense);

    PathTransform t;
    t.yawRad = 35.0 * kDeg2Rad;
    t.extent = 0.7;
    std::vector<Vec3> drawn;
    transformedNodes(s, centre, t, drawn);

    std::size_t visible = drawn.size();
    for (std::size_t i = 0; i < drawn.size(); ++i)
        if (toScreen(Projection::Globe, camera, vp, drawn[i]).depth > 0.0) visible = i;
    REQUIRE(visible < drawn.size());

    const ScreenPoint at = toScreen(Projection::Globe, camera, vp, drawn[visible]);
    CHECK(pickNode(Projection::Globe, camera, vp, drawn, at.x, at.y) == static_cast<int>(visible));

    //  Untransformed positions are somewhere else entirely: picking those would be picking the invisible.
    const ScreenPoint authored = toScreen(Projection::Globe, camera, vp, s.nodes[visible].p);
    CHECK(std::hypot(authored.x - at.x, authored.y - at.y) > kNodeHitPixels);

    //  And the drag comes back to the authored point it was drawn from.
    const Vec3 back = unapplyTransform(drawn[visible], centre, t);
    CHECK(sameDir(back, s.nodes[visible].p, 1e-9));
}

TEST_CASE("the curve is offered only where it faces you") {
    const TrajectoryState s = customChain();
    const Camera camera = cameraFor(ViewPreset::Front);
    const Viewport vp = testViewport();
    const Vec3 centre{0.0, 0.0, 1.0};
    const PathTransform none;
    const auto samples = samplePath(s);

    //  A sample on the far side whose screen position is nowhere near the near side: on a globe the two
    //  hemispheres land on one disc, so a back point can sit under a front one, and that is not the case
    //  being tested here.
    const PathSample* behind = nullptr;
    for (const auto& sample : samples) {
        const ScreenPoint at = toScreen(Projection::Globe, camera, vp, sample.p);
        if (at.depth > 0.0) continue;
        double nearestFront = 1e9;
        for (const auto& other : samples) {
            const ScreenPoint p = toScreen(Projection::Globe, camera, vp, other.p);
            if (p.depth > 0.0) nearestFront = std::min(nearestFront, std::hypot(p.x - at.x, p.y - at.y));
        }
        if (nearestFront > 3.0 * kInsertHitPixels) {
            behind = &sample;
            break;
        }
    }
    REQUIRE(behind != nullptr);

    const ScreenPoint hidden = toScreen(Projection::Globe, camera, vp, behind->p);
    CHECK_FALSE(nearestOnCurveScreen(Projection::Globe, camera, vp, s, centre, none, hidden.x, hidden.y).found);

    //  The equirect hides nothing, so the same part of the curve is reachable there.
    const ScreenPoint flat = toScreen(Projection::Equirect, camera, vp, behind->p);
    CHECK(nearestOnCurveScreen(Projection::Equirect, camera, vp, s, centre, none, flat.x, flat.y).found);
}

TEST_CASE("an insertion lands where the click was, not on the nearest sample") {
    const TrajectoryState s = customChain();
    const Camera camera = cameraFor(ViewPreset::Front);
    //  Magnified: the dense samples are half a pixel apart on a 120-pixel radius, and interpolating across
    //  half a pixel asserts nothing. At this size they are about eight.
    Viewport vp = testViewport();
    vp.rx = vp.ry = 2000.0;
    const Vec3 centre{0.0, 0.0, 1.0};
    const PathTransform none;
    const auto samples = samplePath(s);

    //  The widest pair inside one segment, both ends facing the viewer: the click goes exactly between them.
    std::size_t best = samples.size();
    double widest = 0.0;
    for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
        if (samples[i].segment != samples[i + 1].segment) continue;
        const ScreenPoint a = toScreen(Projection::Globe, camera, vp, samples[i].p);
        const ScreenPoint b = toScreen(Projection::Globe, camera, vp, samples[i + 1].p);
        if (a.depth <= 0.0 || b.depth <= 0.0) continue;
        const double span = std::hypot(a.x - b.x, a.y - b.y);
        if (span > widest) {
            widest = span;
            best = i;
        }
    }
    REQUIRE(best < samples.size());
    REQUIRE(widest > 2.0);

    const ScreenPoint a = toScreen(Projection::Globe, camera, vp, samples[best].p);
    const ScreenPoint b = toScreen(Projection::Globe, camera, vp, samples[best + 1].p);
    const CurveHit hit =
        nearestOnCurveScreen(Projection::Globe, camera, vp, s, centre, none, 0.5 * (a.x + b.x), 0.5 * (a.y + b.y));
    REQUIRE(hit.found);
    CHECK(hit.segment == samples[best].segment);

    //  Halfway between two samples, in the Bezier parameter as on the screen. Snapping to either sample
    //  instead would be half a sample out, which is exactly what this margin refuses.
    const double spacing = std::abs(samples[best + 1].t - samples[best].t);
    const double midpoint = 0.5 * (samples[best].t + samples[best + 1].t);
    REQUIRE(spacing > 1e-9);
    CHECK(std::abs(hit.t - midpoint) < 0.4 * spacing);
}

namespace {
/// A width's coverage at a pixel, 0 outside its grid.
float coverageAt(const CapCoverage& c, const std::vector<float>& cells, double x, double y) {
    const int col = static_cast<int>(std::floor((x - c.x0) / c.cell));
    const int row = static_cast<int>(std::floor((y - c.y0) / c.cell));
    if (cells.empty() || col < 0 || row < 0 || col >= c.cols || row >= c.rows) return 0.0f;
    return cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(c.cols) + static_cast<std::size_t>(col)];
}
}  // namespace

TEST_CASE("a source's width covers what lies within its angle of the source, and nothing past it") {
    const Viewport vp{100.0, 100.0, 90.0, 90.0};
    const Camera front = cameraFor(ViewPreset::Front);
    CapCoverage c;
    capCoverage(Projection::Globe, front, vp, fromAzEl(0.0, 0.0), 30.0 * kDeg2Rad, 2, c);
    REQUIRE(c.cols > 0);
    CHECK(coverageAt(c, c.front, 100.0, 100.0) == doctest::Approx(1.0));
    CHECK(coverageAt(c, c.front, 100.0 - 90.0 * std::sin(20.0 * kDeg2Rad), 100.0) ==
          doctest::Approx(1.0));  // 20 degrees off
    CHECK(coverageAt(c, c.front, 100.0 - 90.0 * std::sin(40.0 * kDeg2Rad), 100.0) ==
          doctest::Approx(0.0));  // 40 degrees off
    CHECK(coverageAt(c, c.front, 100.0, 100.0 - 90.0 * std::sin(20.0 * kDeg2Rad)) ==
          doctest::Approx(1.0));  // above, too
    float behind = 0.0f;
    for (float v : c.back) behind = std::max(behind, v);
    CHECK(behind == 0.0f);            // a cap facing the viewer has no far side
    CHECK(c.cols * c.cell < 2 * 90);  // and only its own neighbourhood was sampled

    capCoverage(Projection::Globe, front, vp, fromAzEl(0.0, 0.0), 0.0, 2, c);
    CHECK(c.cols == 0);  // no width, nothing to draw
}

TEST_CASE("a width on the globe's edge reaches the edge, on both sides, instead of flattening to a line") {
    //  The case an ellipse gets wrong: seen edge-on, the cap's own plane projects to a line, while the cap
    //  itself bulges round the edge to the front and to the back.
    const Viewport vp{100.0, 100.0, 90.0, 90.0};
    CapCoverage c;
    capCoverage(Projection::Globe, cameraFor(ViewPreset::Front), vp, fromAzEl(kPi / 2.0, 0.0), 30.0 * kDeg2Rad, 2, c);
    const double nearEdge = 100.0 - 0.95 * 90.0;  // 18 degrees from the source, front and back
    CHECK(coverageAt(c, c.front, nearEdge, 100.0) == doctest::Approx(1.0));
    CHECK(coverageAt(c, c.back, nearEdge, 100.0) == doctest::Approx(1.0));
    CHECK(coverageAt(c, c.front, 100.0 - 0.7 * 90.0, 100.0) == doctest::Approx(0.0));  // 46 degrees: outside
}

TEST_CASE("in the equirect a width wraps across the back seam and over a pole") {
    const Viewport vp{200.0, 100.0, 180.0, 90.0};
    const Camera camera{};
    CapCoverage c;
    capCoverage(Projection::Equirect, camera, vp, fromAzEl(kPi, 0.0), 20.0 * kDeg2Rad, 2, c);
    CHECK(coverageAt(c, c.front, 200.0 - 175.0, 100.0) == doctest::Approx(1.0));  // azimuth 175, one edge
    CHECK(coverageAt(c, c.front, 200.0 + 175.0, 100.0) == doctest::Approx(1.0));  // -175, the other
    CHECK(coverageAt(c, c.front, 200.0, 100.0) == doctest::Approx(0.0));          // the front is far away
    CHECK(c.back.empty());

    capCoverage(Projection::Equirect, camera, vp, Vec3{0.0, 0.0, 1.0}, 20.0 * kDeg2Rad, 2, c);
    for (const double x : {40.0, 200.0, 360.0})
        CHECK(coverageAt(c, c.front, x, 100.0 - 90.0 * (80.0 / 90.0)) ==
              doctest::Approx(1.0));  // elevation 80, all round
    CHECK(coverageAt(c, c.front, 200.0, 100.0 - 90.0 * (60.0 / 90.0)) == doctest::Approx(0.0));  // elevation 60
}

TEST_CASE("a width of 180 degrees is the whole sphere, and 90 a hemisphere") {
    const Viewport vp{100.0, 100.0, 90.0, 90.0};
    const Camera front = cameraFor(ViewPreset::Front);
    CapCoverage c;
    capCoverage(Projection::Globe, front, vp, fromAzEl(0.0, 0.0), kPi, 2, c);
    CHECK(coverageAt(c, c.front, 100.0, 100.0) == doctest::Approx(1.0));
    CHECK(coverageAt(c, c.back, 100.0, 100.0) == doctest::Approx(1.0));  // the antipode too

    capCoverage(Projection::Globe, front, vp, fromAzEl(0.0, 0.0), kPi / 2.0, 2, c);
    CHECK(coverageAt(c, c.front, 100.0 - 0.9 * 90.0, 100.0) == doctest::Approx(1.0));  // the front hemisphere
    CHECK(coverageAt(c, c.back, 100.0 - 0.5 * 90.0, 100.0) == doctest::Approx(0.0));   // and none of the back
}

TEST_CASE("a drag off the sphere carries on along its rim") {
    //  Catches: the rim point not in the pointer's direction, not on the silhouette, a drag inside
    //  changed, or the equirect not clamped.
    Camera camera;
    camera.yaw = 0.4;
    camera.pitch = 0.3;
    const Vec3 rim = unprojectToRim(Projection::Globe, camera, 2.0, 0.0);  // far right of the disc
    const auto seen = project(Projection::Globe, camera, rim);
    CHECK(length(rim) == doctest::Approx(1.0));  // a point ON the sphere
    //  at the silhouette, and IN FRONT of it: exactly on it, a dragged handle flickered front and back
    CHECK(seen.depth > 0.001);
    CHECK(seen.depth < 0.05);
    CHECK(seen.x == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(seen.y == doctest::Approx(0.0).epsilon(1e-9));
    const Vec3 diag = unprojectToRim(Projection::Globe, camera, -3.0, 3.0);
    const auto d = project(Projection::Globe, camera, diag);
    CHECK(d.x == doctest::Approx(-std::sqrt(0.5)).epsilon(1e-3));
    CHECK(d.y == doctest::Approx(std::sqrt(0.5)).epsilon(1e-3));

    const Vec3 inside = unprojectToRim(Projection::Globe, camera, 0.3, -0.2);
    const auto exact = unproject(Projection::Globe, camera, 0.3, -0.2);
    REQUIRE(exact.has_value());
    CHECK(length(inside - *exact) < 1e-12);

    const Vec3 edge = unprojectToRim(Projection::Equirect, camera, 1.7, -0.5);
    CHECK(std::abs(azimuth(edge)) == doctest::Approx(kPi));  // the back seam, from either side
    CHECK(elevation(edge) == doctest::Approx(-kPi / 4.0));
}
