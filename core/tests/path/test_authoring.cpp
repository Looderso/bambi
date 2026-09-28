// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <string>
#include <vector>

#include "bambi/math/sphere.hpp"
#include "bambi/patch/state.hpp"  // name(GeneratorType): the serialisation helper lives with the patch
#include "bambi/path/authoring.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

TrajectoryState converted(GeneratorType g, int nodes = 16) {
    TrajectoryState s;
    s.generator = g;
    generatorDefaults(g, s.genParams);
    convertToCustom(s, nodes);
    return s;
}

//  A circle of `nodes` nodes on the equator, fitted as any chain is.
FitResult circle(TrajectoryState& s, int nodes) {
    std::vector<Vec3> dense;
    for (int i = 0; i < 1500; ++i) dense.push_back(fromAzEl(i / 1500.0 * 2.0 * kPi, 0.0));
    return fitNodes(s, dense, nodes, true);
}

double handleLength(const TrajectoryState& s, std::size_t i, Handle which) {
    const Node& n = s.nodes[i];
    return arc(n.p, which == Handle::In ? n.cin : n.cout);
}

Vec3 handleDirection(const TrajectoryState& s, std::size_t i, Handle which) {
    const Node& n = s.nodes[i];
    return unit(logMap(n.p, which == Handle::In ? n.cin : n.cout));
}

std::vector<Vec3> denseOf(const TrajectoryState& s) {
    std::vector<Vec3> out;
    for (const auto& x : samplePath(s)) out.push_back(x.p);
    return out;
}

/// Max over A of the nearest distance to B — parameterisation-independent, unlike comparing
/// sample index to sample index.
double hausdorff(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    double worst = 0.0;
    for (const Vec3& p : a) {
        double m = 1e9;
        for (const Vec3& q : b) m = std::min(m, arc(p, q));
        worst = std::max(worst, m);
    }
    return worst;
}

/// Max over A of the distance to the closed polyline through B. Unlike hausdorff above it does not
/// bottom out at B's sample spacing, so it can tell an unmoved curve from a slightly moved one.
double offPolyline(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    double worst = 0.0;
    for (const Vec3& p : a) {
        double m = 1e9;
        for (std::size_t i = 0; i < b.size(); ++i) {
            const Vec3 q0 = b[i], e = b[(i + 1) % b.size()] - q0;
            const double t = clampd(dot(p - q0, e) / std::max(dot(e, e), 1e-30), 0.0, 1.0);
            m = std::min(m, length(p - (q0 + e * t)));
        }
        worst = std::max(worst, m);
    }
    return worst;
}

/// Equal-arc-length placement with span/3 handles: the naive baseline curvature beats.
TrajectoryState naiveFit(const std::vector<Vec3>& dense, int N) {
    const std::size_t n = dense.size();
    std::vector<double> cum(n, 0.0);
    for (std::size_t i = 1; i < n; ++i) cum[i] = cum[i - 1] + arc(dense[i - 1], dense[i]);
    const double total = cum[n - 1] + arc(dense[n - 1], dense[0]);

    std::vector<std::size_t> idx;
    std::size_t j = 0;
    for (int i = 0; i < N; ++i) {
        const double t = total * i / N;
        while (j + 1 < n && cum[j + 1] < t) ++j;
        idx.push_back(j);
    }
    TrajectoryState s;
    s.kind = TrajectoryKind::Custom;
    s.closed = true;
    for (std::size_t q = 0; q < idx.size(); ++q) {
        const std::size_t i = idx[q];
        const double span = std::max(arc(dense[i], dense[idx[(q + 1) % idx.size()]]), 0.1);
        const Vec3 tan = dense[(i + 1) % n] - dense[(i + n - 1) % n];
        Node nd;
        nd.p = dense[i];
        const Vec3 t = unit(tangentAt(nd.p, tan));
        nd.cin = expMap(nd.p, t * -(span / 3));
        nd.cout = expMap(nd.p, t * (span / 3));
        s.nodes.push_back(nd);
    }
    return s;
}

}  // namespace

/*  THE headline authoring property: inserting a node must not move the curve. Anything else
 *  and the editor fights the user on every click. */
TEST_CASE("insertion is non-destructive") {
    TrajectoryState s = converted(GeneratorType::Lissajous);
    Trajectory before;
    before.build(s);
    const std::vector<Vec3> a = denseOf(s);
    const std::size_t nBefore = s.nodes.size();

    const std::size_t added = insertNode(s, 3, 0.37);
    CHECK(added == 4);
    CHECK(s.nodes.size() == nBefore + 1);

    Trajectory after;
    after.build(s);
    const std::vector<Vec3> b = denseOf(s);

    const double relLen = std::abs(after.lengthRad() - before.lengthRad()) / before.lengthRad();
    INFO("arc length changed by ", relLen * 1e6, " ppm");
    CHECK(relLen < 5e-6);

    const double grid = before.lengthRad() / static_cast<double>(a.size());
    CHECK(hausdorff(a, b) < grid);
    CHECK(hausdorff(b, a) < grid);

    //  The two above cannot fall below the sample spacing. This one can: an unmoved curve sits
    //  4e-6 rad off its old samples' polyline, and one wrong handle on the new node puts it 3e-4 off.
    CHECK(offPolyline(b, a) < 2e-5);
    CHECK(offPolyline(a, b) < 2e-5);
}

TEST_CASE("an inserted node lands on the curve, with handles halved") {
    TrajectoryState s = converted(GeneratorType::Orbit, 12);
    const double span = handleLength(s, 3, Handle::Out);

    Trajectory t;
    t.build(s);
    const std::size_t i = insertNode(s, 3, 0.5);

    //  De Casteljau at t=0.5 halves the neighbours' handles, and gives the new node two equal ones.
    //  Ratios with explicit bounds: these are lengths in radians, well below 1, where Approx's
    //  epsilon is an absolute tolerance -- .epsilon(0.15) here once passed any handle at all.
    const double in = handleLength(s, i, Handle::In), out = handleLength(s, i, Handle::Out);
    CHECK(std::abs(handleLength(s, 3, Handle::Out) / span - 0.5) < 0.01);
    CHECK(std::abs(in / out - 1.0) < 0.01);
    CHECK(in < span * 0.5);
    CHECK(s.nodes[i].smooth);
}

TEST_CASE("insertion goes between the actual neighbours, not at the end") {
    TrajectoryState s = converted(GeneratorType::Wave, 12);
    const Vec3 before = s.nodes[5].p, after = s.nodes[6].p;
    const std::size_t i = insertNode(s, 5, 0.5);
    CHECK(i == 6);
    CHECK(arc(s.nodes[5].p, before) < 1e-7);
    CHECK(arc(s.nodes[7].p, after) < 1e-7);
}

TEST_CASE("insertion refuses an out-of-range segment") {
    TrajectoryState s = converted(GeneratorType::Orbit, 8);
    const std::size_t n = s.nodes.size();
    CHECK(insertNode(s, 99, 0.5) == n);
    CHECK(s.nodes.size() == n);
}

/*  The measurement that justified curvature placement in the first place. */
TEST_CASE("curvature placement beats equal arc length on an orbit wave") {
    TrajectoryState src;
    src.generator = GeneratorType::Wave;
    generatorDefaults(GeneratorType::Wave, src.genParams);
    const std::vector<Vec3> dense = denseOf(src);

    for (int n : {12, 16, 24}) {
        TrajectoryState smart = src;
        const FitResult r = convertToCustom(smart, n);

        TrajectoryState naive = naiveFit(dense, n);
        const double naiveDev = hausdorff(denseOf(naive), dense);

        INFO("nodes ", n, ": curvature ", r.maxDeviationRad * kRad2Deg, " deg vs equal-arc ", naiveDev * kRad2Deg,
             " deg");
        CHECK(r.maxDeviationRad * kRad2Deg < 0.5);
        CHECK(r.maxDeviationRad < naiveDev);
    }
}

TEST_CASE("every generator converts to a faithful chain") {
    for (auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                   GeneratorType::Spiral}) {
        TrajectoryState s;
        s.generator = g;
        generatorDefaults(g, s.genParams);
        const FitResult r = convertToCustom(s, 16);
        INFO(std::string(name(g)), ": ", r.nodes, " nodes, ", r.maxDeviationRad * kRad2Deg, " deg");
        CHECK(s.kind == TrajectoryKind::Custom);
        CHECK(s.closed == generatorIsClosed(g));
        CHECK(r.nodes >= 4);
        CHECK(r.maxDeviationRad * kRad2Deg < 1.0);
    }
}

/*  A geodesic has zero curvature, so its discrete curvature is pure noise. Without a floor
 *  every sample looks like a feature and the arc gets runt segments that then cusp. */
TEST_CASE("a straight arc converts without runt segments") {
    TrajectoryState s;
    s.generator = GeneratorType::Arc;
    generatorDefaults(GeneratorType::Arc, s.genParams);
    convertToCustom(s, 16);

    double smallest = 1e9;
    for (std::size_t i = 0; i + 1 < s.nodes.size(); ++i)
        smallest = std::min(smallest, arc(s.nodes[i].p, s.nodes[i + 1].p));
    INFO("smallest segment ", smallest * kRad2Deg, " deg");
    CHECK(smallest * kRad2Deg > 1.0);
}

TEST_CASE("a fitted chain never backtracks on itself") {
    // A handle longer than its segment cusps, and playback walks by arc length, so the
    // backtrack becomes an audible stall.
    for (auto g : {GeneratorType::Arc, GeneratorType::Spiral}) {
        TrajectoryState s;
        s.generator = g;
        generatorDefaults(g, s.genParams);
        convertToCustom(s, 16);

        const std::vector<Vec3> pts = denseOf(s);
        int backtracks = 0;
        for (std::size_t i = 2; i < pts.size(); ++i)
            if (arc(pts[0], pts[i]) < arc(pts[0], pts[i - 1]) - 1e-9) ++backtracks;
        INFO(std::string(name(g)), " backtracks: ", backtracks);
        CHECK(backtracks == 0);
    }
}

TEST_CASE("a fitted circle is actually round") {
    TrajectoryState s;
    const FitResult r = circle(s, 4);
    CHECK(r.nodes == 4);
    Trajectory t;
    t.build(s);
    CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(360.0).epsilon(2e-3));
    for (int i = 0; i < 64; ++i) CHECK(std::abs(t.eval(i / 64.0).z) < 1e-3);  // stays on the equator
}

TEST_CASE("open and close round-trip exactly") {
    TrajectoryState s;
    circle(s, 4);
    Trajectory t;
    t.build(s);
    const double full = t.lengthRad() * kRad2Deg;
    CHECK(full == doctest::Approx(360.0).epsilon(2e-3));

    setClosed(s, false);
    t.build(s);
    const double open = t.lengthRad() * kRad2Deg;
    INFO("open length ", open);
    CHECK(open == doctest::Approx(270.0).epsilon(5e-3));  // exactly the closing segment gone

    insertNode(s, 1, 0.5);
    t.build(s);
    CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(open).epsilon(1e-3));

    setClosed(s, true);
    t.build(s);
    CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(full).epsilon(5e-3));
}

TEST_CASE("an open path's outer handles are inert") {
    TrajectoryState s;
    circle(s, 5);
    CHECK(handleIsLive(s, 0, Handle::In));
    setClosed(s, false);
    CHECK_FALSE(handleIsLive(s, 0, Handle::In));
    CHECK(handleIsLive(s, 0, Handle::Out));
    CHECK(handleIsLive(s, 4, Handle::In));
    CHECK_FALSE(handleIsLive(s, 4, Handle::Out));
    CHECK(handleIsLive(s, 2, Handle::In));
}

TEST_CASE("handles: smooth mirrors, corner does not") {
    TrajectoryState s;
    circle(s, 6);
    const std::size_t i = 2;

    SUBCASE("smooth keeps the two collinear while each keeps its own length") {
        makeSmooth(s, i);
        const double lenIn = handleLength(s, i, Handle::In);
        setHandle(s, i, Handle::Out, fromAzEl(1.0, 0.5));
        CHECK(dot(handleDirection(s, i, Handle::In), handleDirection(s, i, Handle::Out)) ==
              doctest::Approx(-1.0).epsilon(1e-6));
        CHECK(handleLength(s, i, Handle::In) == doctest::Approx(lenIn).epsilon(1e-6));
    }

    SUBCASE("corner leaves the opposite handle untouched") {
        makeCorner(s, i);
        const Vec3 before = handleDirection(s, i, Handle::In);
        setHandle(s, i, Handle::Out, fromAzEl(1.0, 0.5));
        CHECK(arc(before, handleDirection(s, i, Handle::In)) < 1e-7);
        CHECK_FALSE(s.nodes[i].smooth);
    }
}

TEST_CASE("handle length is clamped in interactive editing") {
    TrajectoryState s;
    circle(s, 4);
    const Vec3 p = s.nodes[1].p, along = unit(tangentAt(p, {0, 0, 1}));
    setHandle(s, 1, Handle::Out, expMap(p, along * 2.5));  // far past the longest handle
    CHECK(handleLength(s, 1, Handle::Out) == doctest::Approx(kHandleMaxRad));
    setHandle(s, 1, Handle::Out, expMap(p, along * 1e-4));  // nearly on the node
    CHECK(handleLength(s, 1, Handle::Out) == doctest::Approx(kHandleMinRad));
}

TEST_CASE("moving a node carries its handles rigidly") {
    TrajectoryState s;
    circle(s, 5);
    const std::size_t i = 2;
    const double lIn = handleLength(s, i, Handle::In), lOut = handleLength(s, i, Handle::Out);
    const double between = arc(s.nodes[i].cin, s.nodes[i].cout);

    moveNode(s, i, fromAzEl(2.0, 0.6));
    CHECK(arc(s.nodes[i].p, fromAzEl(2.0, 0.6)) < 1e-7);
    CHECK(handleLength(s, i, Handle::In) == doctest::Approx(lIn).epsilon(1e-9));
    CHECK(handleLength(s, i, Handle::Out) == doctest::Approx(lOut).epsilon(1e-9));
    CHECK(arc(s.nodes[i].cin, s.nodes[i].cout) == doctest::Approx(between).epsilon(1e-9));
}

TEST_CASE("delete keeps at least two nodes") {
    TrajectoryState s;
    circle(s, 4);
    CHECK(deleteNode(s, 1));
    CHECK(s.nodes.size() == 3);
    CHECK(deleteNode(s, 0));
    CHECK(s.nodes.size() == 2);
    CHECK_FALSE(deleteNode(s, 0));
    CHECK(s.nodes.size() == 2);
    CHECK_FALSE(deleteNode(s, 99));
}

TEST_CASE("makeStart rotates the seam without changing the shape") {
    TrajectoryState s;
    circle(s, 6);
    Trajectory before;
    before.build(s);
    const std::vector<Vec3> a = denseOf(s);
    const Vec3 wasThird = s.nodes[3].p;

    makeStart(s, 3);
    CHECK(arc(s.nodes[0].p, wasThird) < 1e-7);
    CHECK(s.nodes.size() == 6);

    Trajectory after;
    after.build(s);
    CHECK(after.lengthRad() == doctest::Approx(before.lengthRad()).epsilon(1e-6));
    CHECK(hausdorff(denseOf(s), a) * kRad2Deg < 0.5);
}

TEST_CASE("authoring operations tolerate degenerate input") {
    TrajectoryState s;  // no nodes at all
    CHECK(insertNode(s, 0, 0.5) == 0);
    CHECK_FALSE(deleteNode(s, 0));
    CHECK_FALSE(handleIsLive(s, 0, Handle::In));
    moveNode(s, 5, {0, 0, 1});
    setHandle(s, 5, Handle::In, {0, 0, 1});
    makeSmooth(s, 5);
    makeStart(s, 5);
    CHECK(s.nodes.empty());
}

/*  They are Bezier nodes: a handful already makes a complex spline, and past two dozen a path
 *  is not more expressive, only harder to edit. Every way a chain can grow has
 *  to respect that, or the cap is only a suggestion. */
TEST_CASE("a custom trajectory never exceeds the node cap") {
    SUBCASE("insertion is refused at the cap, and changes nothing") {
        TrajectoryState s;
        circle(s, 4);
        while (s.nodes.size() < static_cast<std::size_t>(kMaxNodes)) {
            const std::size_t before = s.nodes.size();
            insertNode(s, 0, 0.5);
            REQUIRE(s.nodes.size() == before + 1);
        }

        const std::vector<Node> before = s.nodes;
        const std::size_t got = insertNode(s, 3, 0.5);
        CHECK(got == before.size());  // the refusal insertNode already had
        REQUIRE(s.nodes.size() == before.size());
        //  Compared exactly, not with arc(): nothing may have moved at all, and arc() cannot
        //  resolve below ~1.5e-8 rad.
        for (std::size_t i = 0; i < before.size(); ++i) {
            CHECK(sameDir(s.nodes[i].p, before[i].p));
            CHECK(sameDir(s.nodes[i].cin, before[i].cin));
            CHECK(sameDir(s.nodes[i].cout, before[i].cout));
        }
    }

    SUBCASE("a fit asked for more nodes gets the cap") {
        const TrajectoryState s = converted(GeneratorType::Lissajous, 60);
        CHECK(s.nodes.size() <= static_cast<std::size_t>(kMaxNodes));
        CHECK(s.nodes.size() >= 2);

        TrajectoryState c;
        circle(c, 40);
        CHECK(c.nodes.size() <= static_cast<std::size_t>(kMaxNodes));
    }
}
