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

/// Ratio of largest to smallest angular step per equal step of s. 1.0 is perfect.
double speedRatio(const Trajectory& t, int k = 400) {
    std::vector<double> d;
    d.reserve(static_cast<std::size_t>(k));
    for (int i = 0; i < k; ++i)
        d.push_back(arc(t.eval(static_cast<double>(i) / k), t.eval(static_cast<double>(i + 1) / k)));
    const auto [lo, hi] = std::minmax_element(d.begin(), d.end());
    return *lo > 0 ? *hi / *lo : 1e9;
}

const GeneratorType kAll[] = {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                              GeneratorType::Spiral};

}  // namespace

/*  THE load-bearing test. Everything about the plugin's premise — loudness driving speed —
 *  rests on equal steps of s being equal steps of arc length. */
TEST_CASE("arc-length parameterisation holds for every generator") {
    for (auto g : kAll) {
        Trajectory t;
        t.build(parametric(g));
        INFO("generator ", std::string(name(g)));
        REQUIRE_FALSE(t.empty());
        CHECK(speedRatio(t) < 1.01);
    }
}

TEST_CASE("arc-length parameterisation holds for a custom bezier chain") {
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
        // A fixed sample count per segment would fail here: uniform Bezier parameter is not
        // uniform arc length, and a long handle is where that bites hardest.
        s.nodes[2].cout = expMap(s.nodes[2].p, logMap(s.nodes[2].p, s.nodes[2].cout) * 4.0);
        Trajectory t2;
        t2.build(s);
        CHECK(speedRatio(t2) < 1.05);
    }
}

TEST_CASE("a great circle measures exactly 360 degrees") {
    TrajectoryState s = parametric(GeneratorType::Orbit);
    s.genParams[0] = 0;   // no tilt
    s.genParams[2] = 90;  // full aperture
    Trajectory t;
    t.build(s);
    CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(360.0).epsilon(1e-4));
    CHECK(t.closed());
}

TEST_CASE("generators declare closedness, and the trajectory honours it") {
    CHECK(generatorIsClosed(GeneratorType::Orbit));
    CHECK(generatorIsClosed(GeneratorType::Lissajous));
    CHECK(generatorIsClosed(GeneratorType::Wave));
    CHECK_FALSE(generatorIsClosed(GeneratorType::Arc));
    CHECK_FALSE(generatorIsClosed(GeneratorType::Spiral));

    for (auto g : kAll) {
        Trajectory t;
        t.build(parametric(g));
        INFO(std::string(name(g)));
        CHECK(t.closed() == generatorIsClosed(g));
    }
}

TEST_CASE("a closed path joins up; an open one does not") {
    Trajectory closed;
    closed.build(parametric(GeneratorType::Lissajous));
    CHECK(arc(closed.eval(0.0), closed.eval(1.0)) < 1e-6);

    Trajectory open;
    open.build(parametric(GeneratorType::Arc));
    CHECK(arc(open.eval(0.0), open.eval(1.0)) * kRad2Deg == doctest::Approx(90.0).epsilon(1e-3));
}

TEST_CASE("an arc spans exactly its declared length") {
    TrajectoryState s = parametric(GeneratorType::Arc);
    for (double deg : {15.0, 90.0, 180.0, 270.0}) {
        s.genParams[2] = deg;
        Trajectory t;
        t.build(s);
        INFO("length ", deg);
        CHECK(t.lengthRad() * kRad2Deg == doctest::Approx(deg).epsilon(1e-3));
    }
}

TEST_CASE("movement modes are a phase mapping and nothing else") {
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
        CHECK(worst == doctest::Approx(0.01).epsilon(1e-6));  // exactly the step size
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
    Trajectory t;
    t.build(parametric(GeneratorType::Wave));
    for (double s : {-1000.0, -0.5, 0.0, 0.5, 1.0, 1.5, 1000.0}) {
        INFO("s = ", s);
        CHECK(length(t.eval(s)) == doctest::Approx(1.0).epsilon(1e-9));
    }
}

TEST_CASE("degenerate states do not crash and report empty") {
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
    //  The case that rules out the obvious implementation. An equatorial orbit is the most
    //  ordinary trajectory in this plugin, and the mean of its points is exactly nothing.
    std::vector<Vec3> equator;
    for (int i = 0; i < 256; ++i) {
        const double a = 2.0 * kPi * i / 256.0;
        equator.push_back(fromAzEl(a, 0.0));
    }
    Vec3 mean{0, 0, 0};
    for (auto p : equator) mean = mean + p;
    REQUIRE(length(mean) < 1e-9);  // the mean really is degenerate

    const Vec3 c = pathCentre(equator);
    CHECK(length(c) == doctest::Approx(1.0));
    CHECK(std::abs(std::abs(c.z) - 1.0) < 1e-6);  // a pole, either one
}

TEST_CASE("a small circle's centre is its own middle, on the side the path is on") {
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
}

TEST_CASE("extent shrinks a path toward its centre without moving it") {
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
    for (auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
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
    std::vector<Vec3> ring;
    for (int i = 0; i < 64; ++i) ring.push_back(fromAzEl(2.0 * kPi * i / 64.0, 0.4));
    const Vec3 c = pathCentre(ring);

    PathTransform t;
    t.yawRad = 30.0 * kDeg2Rad;
    for (auto p : ring) {
        const Vec3 q = applyTransform(p, c, t);
        //  A yaw adds exactly its own angle to every point's azimuth and touches no
        //  elevation. That is what "rotates around the listener" has to mean.
        CHECK(elevation(q) == doctest::Approx(elevation(p)).epsilon(1e-9));
        double d = azimuth(q) - azimuth(p);
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        CHECK(d == doctest::Approx(t.yawRad).epsilon(1e-9));
    }
}

TEST_CASE("pitch tilts the path upward") {
    const Vec3 front = fromAzEl(0.0, 0.0);
    std::vector<Vec3> one{front};
    PathTransform t;
    t.pitchRad = 25.0 * kDeg2Rad;
    const Vec3 q = applyTransform(front, pathCentre(one), t);
    CHECK(elevation(q) * kRad2Deg == doctest::Approx(25.0).epsilon(1e-9));
    CHECK(std::abs(azimuth(q)) < 1e-9);
}

TEST_CASE("transform preserves arc length ratios — it places, it does not distort") {
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
        CHECK(after == doctest::Approx(before).epsilon(1e-9));
    }
}

/*  A node is authored untransformed and drawn through the transform, so editing it needs the way back.
    Extent is the one part that cannot always be undone -- it slerps toward the centre -- which is why
    kEditableExtent exists rather than a guess. */
TEST_CASE("the placement transform can be undone, except where extent has thrown information away") {
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
    for (const auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                         GeneratorType::Spiral}) {
        INFO("shape ", static_cast<int>(g));
        CHECK(built(g, {}).points().size() == static_cast<std::size_t>(kMinLutSize));
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
