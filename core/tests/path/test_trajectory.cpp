// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <string>
#include <vector>

#include "bambi/math/sphere.hpp"
#include "bambi/patch/state.hpp"  // name(GeneratorType): the serialisation helper lives with the patch
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

TrajectoryState parametric(GeneratorType g) {
    TrajectoryState s;
    s.kind = TrajectoryKind::Parametric;
    s.generator = g;
    generatorDefaults(g, s.genParams);
    return s;
}

/// Ratio of largest to smallest angular step per equal step of s. 1.0 is perfect. The default count divides
/// by every polygon's sides, so no step straddles a corner, where the chord is shorter than the path.
double speedRatio(const Trajectory& t, int k = 840) {
    std::vector<double> d;
    d.reserve(static_cast<std::size_t>(k));
    for (int i = 0; i < k; ++i)
        d.push_back(arc(t.eval(static_cast<double>(i) / k), t.eval(static_cast<double>(i + 1) / k)));
    const auto [lo, hi] = std::minmax_element(d.begin(), d.end());
    return *lo > 0 ? *hi / *lo : 1e9;
}

const GeneratorType kAll[] = {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave,
                              GeneratorType::Polygon, GeneratorType::Spiral};

}  // namespace

/*  THE load-bearing test. Everything about the plugin's premise — loudness driving speed —
 *  rests on equal steps of s being equal steps of arc length. */
TEST_CASE("arc-length parameterisation holds for every generator") {
    //  Catches: the table resampled by sample index rather than by arc length.
    for (auto g : kAll) {
        Trajectory t;
        t.build(parametric(g));
        INFO("generator ", std::string(name(g)));
        REQUIRE_FALSE(t.empty());
        CHECK(speedRatio(t) < 1.01);
    }
}

TEST_CASE("arc-length parameterisation holds for a custom bezier chain") {
    //  Catches: the table resampled by sample index rather than by arc length, which a long handle shows at once.
    TrajectoryState s;
    s.kind = TrajectoryKind::Custom;
    s.closed = true;
    for (int i = 0; i < 5; ++i) {
        const double az = i * 72.0 * kDeg2Rad;
        Node n;
        n.p = fromAzEl(az, (i % 2 ? 30.0 : -20.0) * kDeg2Rad);
        const Vec3 tan = unit(tangentAt(n.p, cross({0, 0, 1}, n.p)));
        n.cin = expMap(n.p, tan * -0.35);
        n.cout = expMap(n.p, tan * 0.35);
        s.nodes.push_back(n);
    }
    Trajectory t;
    t.build(s);
    CHECK(speedRatio(t) < 1.02);

    SUBCASE("and still holds when one handle is stretched far out") {
        // Uniform Bezier parameter is not uniform arc length, and a long handle is where that bites hardest.
        s.nodes[2].cout = expMap(s.nodes[2].p, logMap(s.nodes[2].p, s.nodes[2].cout) * 4.0);
        Trajectory t2;
        t2.build(s);
        CHECK(speedRatio(t2) < 1.05);
    }
}

TEST_CASE("a great circle measures exactly 360 degrees") {
    //  Catches: a closed path's length without its closing step.
    TrajectoryState s = parametric(GeneratorType::Orbit);
    s.genParams[0] = 0;   // no tilt
    s.genParams[2] = 90;  // full aperture
    Trajectory t;
    t.build(s);
    CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(360.0).epsilon(1e-4));
    CHECK(t.closed());
}

TEST_CASE("generators declare closedness, and the trajectory honours it") {
    //  Catches: a closed shape declared open, and a trajectory that takes its closedness from the state rather than the
    //  generator.
    CHECK(generatorIsClosed(GeneratorType::Orbit));
    CHECK(generatorIsClosed(GeneratorType::Lissajous));
    CHECK(generatorIsClosed(GeneratorType::Wave));
    CHECK(generatorIsClosed(GeneratorType::Polygon));
    CHECK_FALSE(generatorIsClosed(GeneratorType::Spiral));

    for (auto g : kAll) {
        Trajectory t;
        t.build(parametric(g));
        INFO(std::string(name(g)));
        CHECK(t.closed() == generatorIsClosed(g));
    }
}

TEST_CASE("a closed path joins up; an open one does not") {
    //  Catches: every shape taken as closed, which joins a spiral's ends, and every one as open, which stops a loop
    //  short of its start.
    Trajectory closed;
    closed.build(parametric(GeneratorType::Lissajous));
    CHECK(arc(closed.eval(0.0), closed.eval(1.0)) < 1e-6);

    Trajectory open;
    open.build(parametric(GeneratorType::Spiral));  // from 80 up to 80 down, ending at the azimuth it started on
    CHECK(arc(open.eval(0.0), open.eval(1.0)) * kRad2Deg == doctest::Approx(160.0).epsilon(1e-3));
}

TEST_CASE("a polygon's corners sit at its radius, and its edges are great-circle arcs") {
    //  Catches: corners placed off the radius, and edges interpolated in azimuth and elevation, which bow
    //  off the great circle and make the path longer than its sides.
    std::array<double, kMaxGenParams> gp{};
    for (int sides = 2; sides <= 8; ++sides)
        for (double radius : {10.0, 45.0, 80.0}) {
            gp[0] = sides;
            gp[1] = radius;
            gp[2] = 30.0;
            INFO(sides, " sides, radius ", radius);
            for (int k = 0; k < sides; ++k) {
                const Vec3 c = generatorAt(GeneratorType::Polygon, static_cast<double>(k) / sides, gp);
                CHECK(arc(Vec3{1, 0, 0}, c) * kRad2Deg == doctest::Approx(radius).epsilon(1e-9));
                const Vec3 next = generatorAt(GeneratorType::Polygon, static_cast<double>(k + 1) / sides, gp);
                const Vec3 mid = generatorAt(GeneratorType::Polygon, (k + 0.37) / sides, gp);
                CHECK(std::abs(dot(unit(cross(c, next)), mid)) < 1e-9);
            }
            const double r = radius * kDeg2Rad;
            const double side =
                std::acos(std::cos(r) * std::cos(r) + std::sin(r) * std::sin(r) * std::cos(2 * kPi / sides));
            TrajectoryState s = parametric(GeneratorType::Polygon);
            std::copy(gp.begin(), gp.end(), s.genParams.begin());
            Trajectory t;
            t.build(s);
            CHECK(std::abs(t.lengthRad() / (sides * side) - 1.0) < 2e-3);
        }
}

TEST_CASE("one side is a point that stands still, and still a path") {
    //  Catches: one side read as no path at all, an empty table that leaves the source nowhere to be.
    TrajectoryState s = parametric(GeneratorType::Polygon);
    s.genParams[0] = 1;
    Trajectory t;
    t.build(s);
    CHECK_FALSE(t.empty());
    CHECK(t.lengthRad() == 0.0);
    for (double at : {0.0, 0.3, 0.999, 1.0}) {
        const Vec3 p = t.eval(at);
        CHECK(std::isfinite(p.x));
        CHECK(arc(p, Vec3{1, 0, 0}) < 1e-12);
    }
}

TEST_CASE("a polygon is played at one speed, through its corners too") {
    //  Catches: playback by the generator's own parameter where it is not arc length, which would slow down
    //  or jump at a corner.
    TrajectoryState s = parametric(GeneratorType::Polygon);
    s.genParams[0] = 3;
    s.genParams[1] = 60;
    Trajectory t;
    t.build(s);
    constexpr int steps = 840 * 3;
    double lo = 1e9, hi = 0.0;
    for (int i = 0; i < steps; ++i) {
        const double step = arc(t.eval(static_cast<double>(i) / steps), t.eval(static_cast<double>(i + 1) / steps));
        lo = std::min(lo, step);
        hi = std::max(hi, step);
    }
    INFO("steps from ", lo * kRad2Deg, " to ", hi * kRad2Deg, " deg");
    CHECK(lo / hi > 0.9);
}

TEST_CASE("movement modes are a phase mapping and nothing else") {
    //  Catches: ping-pong wrapping instead of folding, once wrapping instead of holding, a closed path following the
    //  mode, and a negative phase wrapped by fmod.
    SUBCASE("wrap is a sawtooth — it teleports, by design") {
        double worst = 0.0;
        for (int i = 0; i < 400; ++i) {
            const double a = Trajectory::phaseToS(i * 0.01, MovementMode::Wrap, false);
            const double b = Trajectory::phaseToS((i + 1) * 0.01, MovementMode::Wrap, false);
            worst = std::max(worst, std::abs(b - a));
        }
        CHECK(worst > 0.9);
    }

    SUBCASE("ping-pong keeps position continuous and flips only velocity") {
        double worst = 0.0;
        for (int i = 0; i < 400; ++i) {
            const double a = Trajectory::phaseToS(i * 0.01, MovementMode::PingPong, false);
            const double b = Trajectory::phaseToS((i + 1) * 0.01, MovementMode::PingPong, false);
            worst = std::max(worst, std::abs(b - a));
        }
        CHECK(std::abs(worst - 0.01) < 1e-9);  // exactly the step size
    }

    SUBCASE("ping-pong folds into range and reverses") {
        CHECK(Trajectory::phaseToS(0.0, MovementMode::PingPong, false) == doctest::Approx(0.0));
        CHECK(Trajectory::phaseToS(0.5, MovementMode::PingPong, false) == doctest::Approx(0.5));
        CHECK(Trajectory::phaseToS(1.0, MovementMode::PingPong, false) == doctest::Approx(1.0));
        CHECK(Trajectory::phaseToS(1.5, MovementMode::PingPong, false) == doctest::Approx(0.5));
        CHECK(Trajectory::phaseToS(2.0, MovementMode::PingPong, false) == doctest::Approx(0.0));
    }

    SUBCASE("once ramps and holds") {
        CHECK(Trajectory::phaseToS(0.4, MovementMode::Once, false) == doctest::Approx(0.4));
        CHECK(Trajectory::phaseToS(1.0, MovementMode::Once, false) == doctest::Approx(1.0));
        CHECK(Trajectory::phaseToS(9.0, MovementMode::Once, false) == doctest::Approx(1.0));
        CHECK(Trajectory::phaseToS(-3.0, MovementMode::Once, false) == doctest::Approx(0.0));
    }

    SUBCASE("a closed path always wraps, whatever the mode says") {
        for (auto m : {MovementMode::Wrap, MovementMode::PingPong, MovementMode::Once})
            CHECK(Trajectory::phaseToS(1.25, m, true) == doctest::Approx(0.25));
    }

    SUBCASE("negative phase wraps correctly rather than going negative") {
        CHECK(Trajectory::phaseToS(-0.25, MovementMode::Wrap, true) == doctest::Approx(0.75));
        const double v = Trajectory::phaseToS(-0.25, MovementMode::PingPong, false);
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
    }
}

TEST_CASE("eval is bounded and unit for any input, including nonsense") {
    //  Catches: eval returning the chord between table points rather than the point on the sphere.
    Trajectory t;
    t.build(parametric(GeneratorType::Wave));
    //  Between the table's points as well as on them: 0.5 of a table whose size is even is one of its points.
    for (double s : {-1000.0, -0.5, 0.0, 0.5, 1.0, 1.5, 1000.0, -3.3, 0.1234567, 2.71}) {
        INFO("s = ", s);
        CHECK(length(t.eval(s)) == doctest::Approx(1.0).epsilon(1e-9));
    }
}

TEST_CASE("degenerate states do not crash and report empty") {
    //  Catches: an empty table read as if it had a point.
    TrajectoryState s;
    s.kind = TrajectoryKind::Custom;

    Trajectory t;
    t.build(s);  // no nodes
    CHECK(t.empty());
    CHECK(length(t.eval(0.3)) == doctest::Approx(1.0));

    s.nodes.resize(1);
    t.build(s);  // one node
    CHECK(length(t.eval(0.3)) == doctest::Approx(1.0));

    s.nodes.resize(3);  // three identical nodes: zero-length path
    t.build(s);
    CHECK(t.lengthRad() == doctest::Approx(0.0));
    CHECK(length(t.eval(0.7)) == doctest::Approx(1.0));
}

TEST_CASE("sphere maths: exp and log are inverses") {
    //  Catches: expMap moving by sin(d) rather than d along the sphere, and logMap returning a unit vector rather than
    //  one of length d.
    const Vec3 p = fromAzEl(0.7, 0.3);
    for (double d = 0.05; d < 2.5; d += 0.3) {
        const Vec3 tan = unit(tangentAt(p, {0, 0, 1})) * d;
        const Vec3 q = expMap(p, tan);
        CHECK(length(q) == doctest::Approx(1.0));
        CHECK(arc(p, q) == doctest::Approx(d));
        const Vec3 back = logMap(p, q);
        CHECK(length(back) == doctest::Approx(d));
        CHECK(arc(expMap(p, back), q) < 1e-7);
    }
}

TEST_CASE("sphere maths: rotations") {
    //  Catches: rotateAToB turning half way, and slerp not uniform in angle.
    const Vec3 a = fromAzEl(0.0, 0.0), b = fromAzEl(1.2, 0.4);
    // 1e-7, not 1e-12: arc() is acos-based and cannot resolve below ~1.5e-8 rad. See vec3.hpp.
    CHECK(arc(rotateAToB(a, a, b), b) < 1e-7);
    CHECK(arc(rotateAToB(a, a, a), a) < 1e-7);

    SUBCASE("rotation preserves angles between vectors") {
        const Vec3 c = fromAzEl(0.3, -0.2);
        const double before = arc(a, c);
        CHECK(arc(rotateAToB(a, a, b), rotateAToB(c, a, b)) == doctest::Approx(before));
    }

    SUBCASE("slerp interpolates along the great circle") {
        const double w = arc(a, b);
        CHECK(arc(a, slerp(a, b, 0.0)) < 1e-7);
        CHECK(arc(b, slerp(a, b, 1.0)) < 1e-7);
        CHECK(arc(a, slerp(a, b, 0.5)) == doctest::Approx(w * 0.5));
    }
}

TEST_CASE("a great circle has a centre, even though its points average to the origin") {
    //  Catches: the mean of the points used, which is the origin for a great circle and falls back to straight up.
    //  The case that rules out the obvious implementation. An orbit is the most ordinary trajectory in this
    //  plugin, and the mean of its points is exactly nothing. Tilted, so that the fallback of straight up is
    //  not also its pole.
    const Vec3 pole = rotateAxis(Vec3{0, 0, 1}, Vec3{1, 0, 0}, 30.0 * kDeg2Rad);
    std::vector<Vec3> circle;
    for (int i = 0; i < 256; ++i) {
        const double a = 2.0 * kPi * i / 256.0;
        circle.push_back(rotateAxis(fromAzEl(a, 0.0), Vec3{1, 0, 0}, 30.0 * kDeg2Rad));
    }
    Vec3 mean{0, 0, 0};
    for (auto p : circle) mean = mean + p;
    REQUIRE(length(mean) < 1e-9);  // the mean really is degenerate

    const Vec3 c = pathCentre(circle);
    CHECK(length(c) == doctest::Approx(1.0));
    CHECK(std::abs(std::abs(dot(c, pole)) - 1.0) < 1e-6);  // its pole, either one
}

TEST_CASE("a small circle's centre is its own middle, on the side the path is on") {
    //  Catches: the area's sign not corrected by the mean, so a path travelled the other way shrinks toward the
    //  antipode.
    std::vector<Vec3> ring;
    const Vec3 axis = fromAzEl(0.6, 0.4);
    for (int i = 0; i < 128; ++i) {
        const double a = 2.0 * kPi * i / 128.0;
        //  a 20 degree cap around `axis`
        Vec3 t = tangentAt(axis, Vec3{0, 0, 1});
        t = rotateAxis(t, axis, a);
        ring.push_back(expMap(axis, t * (20.0 * kDeg2Rad / length(t))));
    }
    const Vec3 c = pathCentre(ring);
    INFO("centre off by ", arc(c, axis) * kRad2Deg, " deg");
    CHECK(sameDir(c, axis, 1e-9));  // and NOT the antipode
    //  and the same travelled the other way round, whose area points the other way
    std::reverse(ring.begin(), ring.end());
    CHECK(sameDir(pathCentre(ring), axis, 1e-9));
}

TEST_CASE("extent shrinks a path toward its centre without moving it") {
    //  Catches: extent applied to the angle other than in proportion.
    std::vector<Vec3> equator;
    for (int i = 0; i < 128; ++i) equator.push_back(fromAzEl(2.0 * kPi * i / 128.0, 0.0));
    const Vec3 c = pathCentre(equator);

    PathTransform t;
    t.extent = 0.25;
    double maxArc = 0.0;
    for (auto p : equator) maxArc = std::max(maxArc, arc(applyTransform(p, c, t), c));
    //  A great circle sits 90 degrees from its pole; at extent 0.25 that becomes 22.5.
    INFO("radius at extent 0.25: ", maxArc * kRad2Deg, " deg");
    CHECK(maxArc * kRad2Deg == doctest::Approx(22.5).epsilon(0.01));

    t.extent = 1.0;
    for (auto p : equator) {
        const Vec3 q = applyTransform(p, c, t);
        CHECK(sameDir(q, p));  // full extent is the identity
    }
}

/*  The default transform must be the identity. No generator's natural centre is at front --
 *  orbit sits at elevation -30, lissajous at azimuth 90, wave at 90/90 -- so reading centre
 *  az/el as an absolute bearing would relocate every authored path the moment the
 *  parameters existed, at their default values, without the user touching anything. */
TEST_CASE("the default transform leaves every generator exactly where it was") {
    //  Catches: a default other than the identity, which moves every path before anyone touches it.
    for (auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Polygon,
                   GeneratorType::Spiral}) {
        TrajectoryState ts;
        ts.kind = TrajectoryKind::Parametric;
        ts.generator = g;
        generatorDefaults(g, ts.genParams);
        Trajectory path;
        path.build(ts);
        const Vec3 c = pathCentre(path.points());
        const PathTransform identity;  // all defaults
        for (auto p : path.points()) {
            INFO("generator ", static_cast<int>(g));
            REQUIRE(sameDir(applyTransform(p, c, identity), p));
        }
    }
}

TEST_CASE("yaw rotates the path around the listener") {
    //  Catches: yaw turning clockwise.
    std::vector<Vec3> ring;
    for (int i = 0; i < 64; ++i) ring.push_back(fromAzEl(2.0 * kPi * i / 64.0, 0.4));
    const Vec3 c = pathCentre(ring);

    PathTransform t;
    t.yawRad = 30.0 * kDeg2Rad;
    for (auto p : ring) {
        const Vec3 q = applyTransform(p, c, t);
        //  A yaw adds exactly its own angle to every point's azimuth and touches no
        //  elevation. That is what "rotates around the listener" has to mean.
        CHECK(std::abs(elevation(q) - elevation(p)) < 1e-9);
        double d = azimuth(q) - azimuth(p);
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        CHECK(std::abs(d - t.yawRad) < 1e-9);
    }
}

TEST_CASE("pitch tilts the path upward") {
    //  Catches: pitch tilting downward.
    const Vec3 front = fromAzEl(0.0, 0.0);
    std::vector<Vec3> one{front};
    PathTransform t;
    t.pitchRad = 25.0 * kDeg2Rad;
    const Vec3 q = applyTransform(front, pathCentre(one), t);
    CHECK(elevation(q) * kRad2Deg == doctest::Approx(25.0).epsilon(1e-9));
    CHECK(std::abs(azimuth(q)) < 1e-9);
}

TEST_CASE("transform preserves arc length ratios — it places, it does not distort") {
    //  Catches: a rotation about an axis that is not a unit vector, which is no longer rigid.
    std::vector<Vec3> ring;
    for (int i = 0; i < 128; ++i) ring.push_back(fromAzEl(2.0 * kPi * i / 128.0, 0.3));
    const Vec3 c = pathCentre(ring);
    PathTransform t;
    t.yawRad = 1.1;
    t.pitchRad = -0.4;
    t.rollRad = 0.8;
    //  Rotation is rigid, so with extent at 1 every step between neighbours is unchanged.
    for (std::size_t i = 1; i < ring.size(); ++i) {
        const double before = arc(ring[i - 1], ring[i]);
        const double after = arc(applyTransform(ring[i - 1], c, t), applyTransform(ring[i], c, t));
        CHECK(std::abs(after / before - 1.0) < 1e-9);
    }
}

/*  A node is authored untransformed and drawn through the transform, so editing it needs the way back.
    Extent is the one part that cannot always be undone -- it slerps toward the centre -- which is why
    kEditableExtent exists rather than a guess. */
TEST_CASE("the placement transform can be undone, except where extent has thrown information away") {
    //  Catches: pitch undone with the wrong sign, and yaw and pitch undone in the order they were applied.
    TrajectoryState s;
    s.generator = GeneratorType::Lissajous;
    generatorDefaults(s.generator, s.genParams);
    std::vector<Vec3> dense;
    for (const auto& x : samplePath(s)) dense.push_back(x.p);
    const Vec3 centre = pathCentre(dense);

    PathTransform t;
    t.yawRad = 40.0 * kDeg2Rad;
    t.pitchRad = -20.0 * kDeg2Rad;
    t.rollRad = 30.0 * kDeg2Rad;
    t.extent = 0.6;

    std::size_t wrong = 0;
    for (const Vec3& p : dense)
        if (!sameDir(unapplyTransform(applyTransform(p, centre, t), centre, t), p, 1e-9)) ++wrong;
    CHECK(wrong == 0);  // component-wise: arc() bottoms out at 1.5e-8 rad and cannot answer this

    //  Identity stays identity, so a path with no transform is edited exactly where it is drawn.
    const PathTransform none;
    CHECK(sameDir(unapplyTransform(dense[7], centre, none), dense[7]));

    //  Collapsed: everything is the centre, and nothing comes back.
    PathTransform flat;
    flat.extent = 0.0;
    CHECK(sameDir(unapplyTransform(applyTransform(dense[3], centre, flat), centre, flat), centre));
    CHECK(kEditableExtent > 0.0);
}

/*  Extent closes the path to a spot and opens it out the other way, which is one control doing both. */
TEST_CASE("extent closes a path toward its centre and opens it out past the far side") {
    const Vec3 centre{0.0, 0.0, 1.0};
    const Vec3 p = fromAzEl(0.0, 60.0 * kDeg2Rad);  // thirty degrees from the centre
    const double theta = arc(centre, p);
    CHECK(theta == doctest::Approx(30.0 * kDeg2Rad));

    PathTransform t;
    t.extent = 1.0;
    CHECK(sameDir(applyTransform(p, centre, t), p, 1e-12));  // as authored

    t.extent = 0.5;
    CHECK(arc(centre, applyTransform(p, centre, t)) == doctest::Approx(0.5 * theta));

    //  Catches: extent above 1 clamped to a no-op instead of opening the path outward.
    t.extent = 2.0;
    CHECK(arc(centre, applyTransform(p, centre, t)) == doctest::Approx(2.0 * theta));

    //  The sphere runs out at half a turn: the point sits at the antipode and stays there.
    t.extent = kMaxExtent;
    const Vec3 far = applyTransform(fromAzEl(0.0, 0.0), centre, t);  // ninety degrees out
    CHECK(arc(centre, far) == doctest::Approx(kPi).epsilon(1e-9));

    //  A point more than a quarter turn out would overshoot the antipode; it stops there instead.
    const Vec3 wide = fromAzEl(0.0, -10.0 * kDeg2Rad);  // a hundred degrees from the centre
    CHECK(arc(centre, wide) == doctest::Approx(100.0 * kDeg2Rad));
    t.extent = 2.0;
    CHECK(arc(centre, applyTransform(wide, centre, t)) == doctest::Approx(kPi).epsilon(1e-9));

    //  And what opened can be closed again: the inverse divides the angle back down.
    t.extent = 1.5;
    const Vec3 opened = applyTransform(p, centre, t);
    CHECK(sameDir(unapplyTransform(opened, centre, t), p, 1e-9));
}

TEST_CASE("the playback table follows the path's length: every default shape as before, a long one without corners") {
    const auto built = [](GeneratorType g, std::initializer_list<std::pair<int, double>> set) {
        TrajectoryState s;
        s.kind = TrajectoryKind::Parametric;
        s.generator = g;
        generatorDefaults(g, s.genParams);
        for (const auto& [i, v] : set) s.genParams[static_cast<std::size_t>(i)] = v;
        Trajectory t;
        t.build(s);
        return t;
    };
    //  Every shape at its defaults keeps the table it always had, so nothing already rendered changes.
    for (const auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Spiral}) {
        INFO("shape ", static_cast<int>(g));
        CHECK(built(g, {}).points().size() == static_cast<std::size_t>(kMinLutSize));
    }
    //  A polygon's table holds a point on every corner. Catches: a table that cuts a corner, slowing the source there.
    for (int sides = 1; sides <= 8; ++sides) {
        const auto size = built(GeneratorType::Polygon, {{0, sides}}).points().size();
        INFO(sides, " sides, ", size, " points");
        CHECK(size >= static_cast<std::size_t>(sides == 1 ? 1 : kMinLutSize));
        CHECK(size % static_cast<std::size_t>(sides) == 0);
    }

    //  A 7:7 Lissajous at full size, over 4000 degrees: a point at least every 0.6 degrees, not every four.
    const Trajectory folded = built(GeneratorType::Lissajous, {{0, 180.0}, {1, 90.0}, {2, 7.0}, {3, 7.0}});
    const auto size = folded.points().size();
    CHECK(size > static_cast<std::size_t>(kMinLutSize));
    CHECK(size <= static_cast<std::size_t>(kMaxLutSize));
    double widest = 0.0;
    for (std::size_t i = 0; i < size; ++i)
        widest = std::max(widest, arc(folded.points()[i], folded.points()[(i + 1) % size]));
    INFO("widest step ", widest * kRad2Deg, " deg over ", size, " points");
    CHECK(widest <= kMaxLutStepRad * 1.01);

    CHECK(lutSizeFor(0.0) == kMinLutSize);
    CHECK(lutSizeFor(1e6) == kMaxLutSize);
}

TEST_CASE("a long shape is sampled as finely as its table, not only as finely as a short one") {
    //  Catches: the second, finer pass dropped, so a long shape's table interpolates between samples four degrees
    //  apart.
    //  Without the second pass a long table would still have short steps -- interpolated between samples
    //  four degrees apart -- so this is asserted on the samples themselves.
    TrajectoryState s;
    s.kind = TrajectoryKind::Parametric;
    s.generator = GeneratorType::Lissajous;
    generatorDefaults(s.generator, s.genParams);
    CHECK(samplePath(s).size() == static_cast<std::size_t>(2 * kMinLutSize));  // a default shape: as it always was

    s.genParams[0] = 180.0, s.genParams[1] = 90.0, s.genParams[2] = 7.0, s.genParams[3] = 7.0;
    Trajectory t;
    t.build(s);
    CHECK(samplePath(s).size() == 2 * t.points().size());
}

TEST_CASE("an orbit's ring reaches every height, overhead included, and a new encoder starts on one") {
    /*  The tilt only turns the orbit's axis down, so with the aperture capped at 90 a small ring could sit
     *  at the floor and never overhead. Past 90 the ring is around the opposite pole: at tilt 90
     *  the aperture is a height, floor to overhead. */
    const auto aperture = generatorParams(GeneratorType::Orbit)[2];
    REQUIRE(aperture.name == "aperture");
    //  Catches the range going back to 90, which is what the UI's control reads.
    CHECK(aperture.max >= 150.0);
    const auto elevationRange = [](double tilt, double ap) {
        const std::array<double, 3> gp{tilt, 0.0, ap};
        double lo = 90.0, hi = -90.0;
        for (int i = 0; i < 360; ++i) {
            const double el =
                std::asin(std::clamp(generatorAt(GeneratorType::Orbit, i / 360.0, gp).z, -1.0, 1.0)) * kRad2Deg;
            lo = std::min(lo, el), hi = std::max(hi, el);
        }
        return std::pair{lo, hi};
    };
    const auto [lo, hi] = elevationRange(90.0, 150.0);
    CHECK(lo == doctest::Approx(60.0).epsilon(1e-6));
    CHECK(hi == doctest::Approx(60.0).epsilon(1e-6));
    //  Catches the default going back: a fresh state is an orbit.
    CHECK(TrajectoryState{}.generator == GeneratorType::Orbit);
}
