// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <vector>

#include "bambi/math/sphere.hpp"
#include "bambi/region/shape.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

/*  Directions spread over the whole sphere, so "in every direction" means something. A Fibonacci
 *  lattice rather than a lat/long grid, which would crowd the poles and under-sample the equator --
 *  exactly where a band's and a sector's edges live. */
std::vector<Vec3> everywhere(int n = 400) {
    std::vector<Vec3> out;
    const double ga = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt(std::max(0.0, 1.0 - z * z));
        out.push_back({r * std::cos(i * ga), r * std::sin(i * ga), z});
    }
    return out;
}

Region spot(double sizeDeg, double softDeg, Vec3 aim = {0, 0, 1}) {
    Region r;
    r.kind = RegionKind::Spot;
    aimAt(r, aim);
    r.size = sizeDeg * kDeg2Rad;
    r.softness = softDeg * kDeg2Rad;
    return r;
}

}  // namespace

TEST_CASE("everywhere is everywhere, and its outside is nowhere") {
    //  Catches: Everywhere falling through to a shape's edge distance, and a side applied before
    //  the kind is looked at.
    Region r;
    for (const Vec3& d : everywhere()) CHECK(valueAt(r, d) == 1.0);
    r.side = RegionSide::Outside;
    for (const Vec3& d : everywhere()) CHECK(valueAt(r, d) == 0.0);
}

TEST_CASE("a spot is 1 at its aim, exactly 0.5 at its edge, and 0 beyond the fade") {
    //  The three points that pin the ramp. Catches: a sign flip in size - theta, a missing +0.5
    //  (the edge would read 0 or 1), and a softness read as a half-width (the edge off-centre).
    const Region r = spot(40.0, 20.0);
    CHECK(valueAt(r, {0, 0, 1}) == doctest::Approx(1.0));
    CHECK(valueAt(r, fromAzEl(0.0, (90.0 - 40.0) * kDeg2Rad)) == doctest::Approx(0.5));
    CHECK(valueAt(r, fromAzEl(0.0, (90.0 - 51.0) * kDeg2Rad)) == doctest::Approx(0.0));
    CHECK(valueAt(r, fromAzEl(0.0, (90.0 - 29.0) * kDeg2Rad)) == doctest::Approx(1.0));
    //  and the fade is monotone in between
    double last = 1.1;
    for (double deg = 28.0; deg <= 52.0; deg += 0.5) {
        const double v = valueAt(r, fromAzEl(0.0, (90.0 - deg) * kDeg2Rad));
        CHECK(v <= last + 1e-12);
        last = v;
    }
}

TEST_CASE("inside + outside is exactly 1, in every direction, for every shape") {
    //  The scalar form of the matrix half's I - M. Exact, not approximate.
    //  Catches: side handling that clamps or rounds after inverting.
    std::vector<Region> shapes;
    shapes.push_back(spot(40.0, 20.0, unit(Vec3{1, 2, -0.5})));
    shapes.push_back(spot(90.0, 180.0));  // the fade: 1 to 0 across the sphere
    {
        Region b;
        b.kind = RegionKind::Band;
        b.bandElevation = 20.0 * kDeg2Rad;
        b.thickness = 35.0 * kDeg2Rad;
        b.softness = 12.0 * kDeg2Rad;
        shapes.push_back(b);
    }
    {
        Region s;
        s.kind = RegionKind::Sectors;
        s.sectors = 4;
        s.fill = 0.6;
        s.softness = 9.0 * kDeg2Rad;
        shapes.push_back(s);
    }
    {
        Region d;
        d.kind = RegionKind::Dots;
        d.dots = 12;
        d.dotSize = 25.0 * kDeg2Rad;
        d.softness = 7.0 * kDeg2Rad;
        shapes.push_back(d);
    }
    for (Region r : shapes)
        for (const Vec3& d : everywhere()) {
            r.side = RegionSide::Inside;
            const double in = valueAt(r, d);
            r.side = RegionSide::Outside;
            const double out = valueAt(r, d);
            CHECK(in + out == 1.0);
            CHECK(in >= 0.0);
            CHECK(in <= 1.0);
        }
}

/*  Catches: the field's matrix the wrong way round (rows for columns, the inverse rotation, which
 *  reads right only for a region that is not turned) and an axis dropped from it. Every kind is
 *  turned about all three axes at once, because a yaw alone is symmetric enough to hide a
 *  transpose for a spot. */
TEST_CASE("a field answers what valueAt does, for every kind however it is turned") {
    for (const auto kind :
         {RegionKind::Everywhere, RegionKind::Spot, RegionKind::Band, RegionKind::Sectors, RegionKind::Dots}) {
        for (const auto side : {RegionSide::Inside, RegionSide::Outside}) {
            Region r;
            r.kind = kind;
            r.side = side;
            r.yaw = 0.7;
            r.pitch = -0.4;
            r.roll = 1.1;
            r.softness = 20.0 * kDeg2Rad;  // soft, so the value is continuous and a rounding is not a flip
            const RegionField field(r);
            double worst = 0.0;
            for (const Vec3& d : everywhere()) worst = std::max(worst, std::abs(field.at(d) - valueAt(r, d)));
            CHECK(worst < 1e-9);
        }
    }
}

TEST_CASE("a hard edge is exactly 0 or 1, and never a nan") {
    //  Softness below kHardEdge must not reach the ramp: dividing by it would give inf, and 0/0 at
    //  the edge itself a nan. Catches: the guard inverted, or removed.
    for (const double soft : {0.0, 0.1, 0.49}) {
        const Region r = spot(40.0, soft);
        for (const Vec3& d : everywhere()) {
            const double v = valueAt(r, d);
            CHECK((v == 0.0 || v == 1.0));
        }
    }
    //  and just above it the ramp is live
    const Region ramp = spot(40.0, 4.0);
    CHECK(valueAt(ramp, fromAzEl(0.0, (90.0 - 40.0) * kDeg2Rad)) == doctest::Approx(0.5));
}

TEST_CASE("aim turns the shape, and nothing else") {
    //  Rotate the aim and the query direction together and the value cannot change.
    //  Catches: the frame applied the wrong way round -- invisible for a shape left where it was
    //  authored, and the classic way to get this wrong.
    const Vec3 axis = unit(Vec3{0.3, -1.0, 0.6});
    for (const double turn : {0.4, 1.3, 2.5, 3.0}) {
        Region a = spot(35.0, 15.0);
        Region b = a;
        aimAt(b, rotateAxis(aimOf(a), axis, turn));
        for (const Vec3& d : everywhere(200))
            CHECK(std::abs(valueAt(a, d) - valueAt(b, rotateAxis(d, axis, turn))) < 1e-12);
    }
    Region down = spot(30.0, 0.0, Vec3{0, 0, -1});
    CHECK(valueAt(down, {0, 0, -1}) == 1.0);
    CHECK(valueAt(down, {0, 0, 1}) == 0.0);
}

TEST_CASE("yaw, pitch and roll are the placement's: front at zero, left, up, and about the axis") {
    //  Closed forms, one per rotation, each of which a flipped sign gets wrong.
    Region r;
    CHECK(arc(aimOf(r), {1, 0, 0}) < 1e-12);
    r.yaw = kPi / 2;
    CHECK(arc(aimOf(r), {0, 1, 0}) < 1e-12);  // CCW from front: left
    r = Region{};
    r.pitch = kPi / 2;
    CHECK(arc(aimOf(r), {0, 0, 1}) < 1e-12);  // front tilts toward up
    r.yaw = 1.1;                              // yaw after pitch: up stays up
    CHECK(arc(aimOf(r), {0, 0, 1}) < 1e-12);

    //  aimOf and toOwnFrame are written separately, and have to be the same frame.
    for (const double yaw : {-2.0, 0.0, 0.7})
        for (const double pitch : {-kPi / 2, -0.4, 0.0, 1.2, 2.9})
            for (const double roll : {0.0, 0.9}) {
                Region q;
                q.yaw = yaw, q.pitch = pitch, q.roll = roll;
                CHECK(arc(toOwnFrame(q, aimOf(q)), {0, 0, 1}) < 1e-12);
            }

    //  Where each kind sits at zero: a spot faces front; a band, sectors and dots stand up, with
    //  sector 0 centred on front. Roll turns about that axis, CCW seen from above: front toward left.
    Region band;
    band.kind = RegionKind::Band;
    CHECK(arc(aimOf(band), {0, 0, 1}) < 1e-12);
    CHECK(arc(aimOf(spot(30.0, 0.0, {1, 0, 0})), {1, 0, 0}) < 1e-12);
    //  Four dots at zero are the A-format tetrahedron: front-left-up, front-right-down,
    //  back-left-down, back-right-up. Faced front instead, it would be its mirror image.
    Region four;
    four.kind = RegionKind::Dots;
    four.dots = 4;
    four.dotSize = 10.0 * kDeg2Rad;
    CHECK(arc(aimOf(four), {0, 0, 1}) < 1e-12);
    CHECK(valueAt(four, unit(Vec3{1, 1, 1})) == 1.0);
    CHECK(valueAt(four, unit(Vec3{1, -1, -1})) == 1.0);
    CHECK(valueAt(four, unit(Vec3{1, 1, -1})) == 0.0);

    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 3;  // odd, so half a turn of roll is not nothing
    s.fill = 0.2;   // narrow: 12 degrees either side of a centre
    const Vec3 left30 = fromAzEl(kPi / 6, 0.0), right30 = fromAzEl(-kPi / 6, 0.0);
    CHECK(valueAt(s, {1, 0, 0}) == 1.0);
    CHECK(valueAt(s, left30) == 0.0);
    s.roll = kPi / 6;
    CHECK(valueAt(s, left30) == 1.0);
    CHECK(valueAt(s, right30) == 0.0);  // and not toward the right
}

TEST_CASE("dragging the aim carries a pattern; it does not spin it") {
    /*  Yaw and pitch alone turn a pattern about its axis on the way to a new aim -- a quarter turn in
        the first half degree off a pole, which is where an upright kind starts. The measure: carry
        the old frame to the new aim along the shortest arc, and ask how far the result is from that. */
    //  The frame is a rotation, so the world direction of the shape's own x is the first row of it.
    const auto ownX = [](const Region& r) {
        return Vec3{toOwnFrame(r, {1, 0, 0}).x, toOwnFrame(r, {0, 1, 0}).x, toOwnFrame(r, {0, 0, 1}).x};
    };
    const auto twist = [&](const Region& before, const Region& after) {
        return arc(rotateAToB(ownX(before), aimOf(before), aimOf(after)), ownX(after));
    };

    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 3;
    //  off the pole, in a direction yaw is not already facing
    Region moved = s;
    aimAt(moved, fromAzEl(kPi / 2, (90.0 - 0.5) * kDeg2Rad));
    CHECK(arc(aimOf(moved), fromAzEl(kPi / 2, 89.5 * kDeg2Rad)) < 1e-9);
    CHECK(twist(s, moved) < 1e-6);  // arc() resolves 1.5e-8; a spin is degrees

    //  passing close beside the pole, step by step
    Region walk = s;
    aimAt(walk, fromAzEl(0.0, 80.0 * kDeg2Rad));
    double worst = 0.0;
    for (int i = 1; i <= 80; ++i) {
        const Region was = walk;
        //  a straight line in the plane above the pole, missing it by one degree
        const double x = std::cos(80.0 * kDeg2Rad) * (1.0 - i / 40.0), y = std::sin(1.0 * kDeg2Rad);
        aimAt(walk, unit(Vec3{x, y, std::sqrt(std::max(0.0, 1.0 - x * x - y * y))}));
        worst = std::max(worst, twist(was, walk));
    }
    CHECK(worst < 1e-6);

    //  grabbing the aim where it is changes nothing, pitched past a quarter turn included
    Region over = s;
    over.yaw = 20.0 * kDeg2Rad, over.pitch = 120.0 * kDeg2Rad, over.roll = 0.3;
    Region held = over;
    aimAt(held, aimOf(over));
    CHECK(std::abs(held.yaw - over.yaw) < 1e-9);
    CHECK(std::abs(held.pitch - over.pitch) < 1e-9);
    CHECK(std::abs(held.roll - over.roll) < 1e-9);

    //  straight up has no azimuth: yaw stays where it was instead of snapping to front
    Region tilted = s;
    tilted.yaw = 1.0, tilted.pitch = -0.3;
    aimAt(tilted, {0, 0, 1});
    CHECK(tilted.yaw == 1.0);
    CHECK(arc(aimOf(tilted), {0, 0, 1}) < 1e-9);

    //  and a kind whose roll cannot be seen keeps the roll it was given
    Region b;
    b.kind = RegionKind::Band;
    b.roll = 0.7;
    aimAt(b, fromAzEl(1.0, 0.4));
    CHECK(b.roll == 0.7);
}

TEST_CASE("the frame is a rotation at every orientation, straight down included") {
    /*  Catches: a frame built as the shortest arc onto up, which leaves the turn about the aim to
        chance and, at straight down, has no axis at all -- a point inversion there would mirror a
        pattern. Spot and band are symmetric about the axis and cannot show it; a rotation keeps
        cross products, a mirror or inversion flips them. */
    const Vec3 a = unit(Vec3{0.2, 0.9, -0.3}), b = unit(Vec3{-0.7, 0.1, 0.6});
    for (const double pitch : {-kPi / 2, -kPi / 2 + 1e-7, 0.0, 0.8, kPi / 2, 2.5})
        for (const double yaw : {0.0, 1.9})
            for (const double roll : {0.0, -1.3}) {
                Region r;
                r.yaw = yaw, r.pitch = pitch, r.roll = roll;
                const Vec3 oa = toOwnFrame(r, a), ob = toOwnFrame(r, b);
                CHECK(length(toOwnFrame(r, cross(a, b)) - cross(oa, ob)) < 1e-12);
                CHECK(std::abs(dot(oa, ob) - dot(a, b)) < 1e-12);
            }

    //  And it is continuous through straight down: three sectors, swept across it in small steps,
    //  never jump at a direction away from their edges' fade.
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 3;
    s.softness = 20.0 * kDeg2Rad;
    const Vec3 probe = unit(Vec3{0.5, 0.4, -0.75});
    double before = -1.0, worst = 0.0;
    for (int i = 0; i <= 400; ++i) {
        s.pitch = (-80.0 - 20.0 * i / 400.0) * kDeg2Rad;
        const double v = valueAt(s, probe);
        if (before >= 0.0) worst = std::max(worst, std::abs(v - before));
        before = v;
    }
    CHECK(worst < 0.01);
}

TEST_CASE("roll turns a pattern about its axis: a whole period is nothing, half of one swaps it") {
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 5;
    s.fill = 0.5;
    Region whole = s, half = s;
    whole.roll = 2.0 * kPi / 5;
    half.roll = kPi / 5;
    for (const Vec3& d : everywhere(300)) {
        CHECK(std::abs(valueAt(s, d) - valueAt(whole, d)) < 1e-9);
        //  hard edges, so away from an edge the half turn is exactly the other side
        const double e = edgeDistance(s, toOwnFrame(s, d));
        if (std::abs(e) > 1e-6) CHECK(valueAt(half, d) == 1.0 - valueAt(s, d));
    }
}

TEST_CASE("a spot of 90 degrees is a half, and aim decides which") {
    //  The presets rely on a half being a spot of 90 degrees.
    const Region front = spot(90.0, 0.0, Vec3{1, 0, 0});
    CHECK(valueAt(front, {1, 0, 0}) == 1.0);
    CHECK(valueAt(front, {-1, 0, 0}) == 0.0);
    CHECK(valueAt(front, {0, 1, 0}) == 1.0);  // exactly on the edge counts as inside
    const Region up = spot(90.0, 0.0, Vec3{0, 0, 1});
    CHECK(valueAt(up, {0, 0, 1}) == 1.0);
    CHECK(valueAt(up, {0, 0, -1}) == 0.0);
}

TEST_CASE("a band is symmetric about its own elevation, and a full one is everywhere") {
    Region b;
    b.kind = RegionKind::Band;
    b.bandElevation = 25.0 * kDeg2Rad;
    b.thickness = 30.0 * kDeg2Rad;
    b.softness = 8.0 * kDeg2Rad;
    /*  First, WHERE it is. Symmetry alone cannot catch a band centred on the wrong latitude -- it is
        symmetric about whatever centre the code picks -- and a mutation that dropped the turn from
        colatitude to elevation slipped through until this was here. */
    {
        Region hard = b;
        hard.softness = 0.0;
        CHECK(valueAt(hard, fromAzEl(0.0, 25.0 * kDeg2Rad)) == 1.0);  // its own elevation
        CHECK(valueAt(hard, fromAzEl(0.0, 39.0 * kDeg2Rad)) == 1.0);  // just inside the top
        CHECK(valueAt(hard, fromAzEl(0.0, 41.0 * kDeg2Rad)) == 0.0);  // just outside it
        CHECK(valueAt(hard, fromAzEl(0.0, 11.0 * kDeg2Rad)) == 1.0);  // just inside the bottom
        CHECK(valueAt(hard, fromAzEl(0.0, 9.0 * kDeg2Rad)) == 0.0);   // just outside it
        CHECK(valueAt(hard, fromAzEl(0.0, 0.0)) == 0.0);              // the horizon is not in it
        CHECK(valueAt(hard, {0, 0, 1}) == 0.0);                       // nor is the zenith
    }
    //  Then, that it is symmetric about that centre.
    for (const double off : {2.0, 7.0, 14.0, 21.0}) {
        const double above = valueAt(b, fromAzEl(0.7, (25.0 + off) * kDeg2Rad));
        const double below = valueAt(b, fromAzEl(0.7, (25.0 - off) * kDeg2Rad));
        CHECK(above == doctest::Approx(below));
    }
    b.thickness = 2.0 * kPi;  // thicker than the sphere
    b.softness = 0.0;
    for (const Vec3& d : everywhere()) CHECK(valueAt(b, d) == 1.0);
}

TEST_CASE("sectors repeat, and fill decides how much of each period is inside") {
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 4;
    s.fill = 0.5;
    s.softness = 0.0;
    //  Periodic in azimuth. Catches: a period taken from the wrong count, and a fold that is not
    //  centred, which would shift every sector by half a period.
    for (double az = -kPi; az < kPi; az += 0.037) {
        const double here = valueAt(s, fromAzEl(az, 0.2));
        const double next = valueAt(s, fromAzEl(az + kPi * 0.5, 0.2));
        CHECK(here == next);
    }
    s.fill = 1.0;
    for (const Vec3& d : everywhere()) CHECK(valueAt(s, d) == 1.0);
    s.fill = 0.0;
    /*  A sector of zero width is its own centre line, where its two edges coincide, and a hard edge
        counts a point exactly on it as inside. So "fill 0 is empty" is true of every direction
        except that line and the aim axis, both of measure zero and neither representable in the
        bus projection. The sweep is turned off the centre lines so it measures the area rather
        than the singularity. */
    CHECK(valueAt(s, fromAzEl(0.0, 0.3)) == 1.0);   // exactly on a centre line
    CHECK(valueAt(s, fromAzEl(1e-6, 0.3)) == 0.0);  // a millionth of a radian off it
    CHECK(valueAt(s, {0, 0, 1}) == 1.0);            // the aim axis, where all four meet
    int inside = 0;
    for (const Vec3& d : everywhere())
        if (valueAt(s, rotateAxis(d, {0, 0, 1}, 0.013)) > 0.0) ++inside;
    CHECK(inside == 0);
}

TEST_CASE("a sector's fade is as wide on the sphere at every latitude") {
    //  The sin(theta) in the sector's edge distance. Without it the fade would be measured in
    //  azimuth, so near the pole -- where a degree of azimuth is a fraction of a degree of arc --
    //  the edge would fade over a band far narrower than the softness asks. Measured as the arc
    //  over which the value runs from 0.9 down to 0.1, walking a circle of latitude.
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 4;
    s.fill = 0.5;
    s.softness = 8.0 * kDeg2Rad;
    const auto fadeArc = [&s](double elevation) {
        double lo = -1.0, hi = -1.0;
        for (double az = 0.0; az < kPi * 0.5; az += 1e-5) {
            const double v = valueAt(s, fromAzEl(az, elevation));
            if (lo < 0.0 && v <= 0.9) lo = az;
            if (v <= 0.1) {
                hi = az;
                break;
            }
        }
        REQUIRE(lo >= 0.0);
        REQUIRE(hi >= 0.0);
        return (hi - lo) * std::cos(elevation);  // azimuth, converted to arc on the sphere
    };
    /*  0.68705 of the softness, because that is where the cap profile passes 0.9 and 0.1 -- a
        linear ramp would give 0.8. Compared as a ratio rather than with doctest::Approx's epsilon
        directly: epsilon scales by (1 + value), so on small radian values it reads as a much looser
        absolute tolerance than its number suggests. */
    const double want = 0.68705 * s.softness;
    for (const double el : {0.0, 30.0, 50.0})
        CHECK(fadeArc(el * kDeg2Rad) / want == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("the edge fades the way a cap blurs it, not in a straight line") {
    /*  The bus applies softness as a blur by a cap of half the softness, one gain per order. This
        is that same blur in closed form -- the fraction of a disc of that radius lying inside a
        straight edge. Checked at the quarter points, where a straight line is furthest from it. */
    const double soft = 20.0 * kDeg2Rad, R = soft * 0.5;
    CHECK(edgeProfile(0.0, soft) == doctest::Approx(0.5));
    CHECK(edgeProfile(R, soft) == 1.0);
    CHECK(edgeProfile(-R, soft) == 0.0);
    CHECK(edgeProfile(2.0 * R, soft) == 1.0);
    CHECK(edgeProfile(0.5 * R, soft) / 0.80450 == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(edgeProfile(-0.5 * R, soft) / 0.19550 == doctest::Approx(1.0).epsilon(1e-4));
    //  a straight line would read 0.75 there: this is what changed
    CHECK(edgeProfile(0.5 * R, soft) > 0.78);
    //  symmetric about the edge, so inside + outside stays exactly 1
    for (double f = -1.0; f <= 1.0; f += 0.05)
        CHECK(edgeProfile(f * R, soft) + edgeProfile(-f * R, soft) == doctest::Approx(1.0).epsilon(1e-12));
    //  monotone
    double last = -1.0;
    for (double f = -1.2; f <= 1.2; f += 0.01) {
        const double v = edgeProfile(f * R, soft);
        CHECK(v >= last - 1e-15);
        last = v;
    }
}

TEST_CASE("what the flat-edge profile costs on a curved one, measured") {
    /*  edgeProfile is the flat-edge case, so error remains where the edge's own curvature is
        comparable to the blur. Measured against a quadrature of the real thing: the fraction of a
        spherical cap of half-angle softness/2, centred on the query direction, that lies inside the
        spot. Stated as a bound, since the exact form is a cap-cap intersection and there is none
        for sectors. */
    const auto exactly = [](double spot, double soft, double d) {
        const double R = soft * 0.5;
        int inside = 0, total = 0;
        //  a deterministic lattice over the cap about +z, then tilted onto the query direction
        for (int i = 1; i <= 60; ++i)
            for (int j = 0; j < 240; ++j) {
                const double z = 1.0 - (1.0 - std::cos(R)) * (i - 0.5) / 60.0;
                const double r = std::sqrt(std::max(0.0, 1.0 - z * z)), phi = 2.0 * kPi * j / 240.0;
                const double th = spot - d;  // where the cap's centre sits, in colatitude
                const Vec3 v{r * std::cos(phi), r * std::sin(phi), z};
                const Vec3 w{v.x * std::cos(th) + v.z * std::sin(th), v.y, -v.x * std::sin(th) + v.z * std::cos(th)};
                ++total;
                if (std::acos(clampd(w.z, -1.0, 1.0)) <= spot) ++inside;
            }
        return static_cast<double>(inside) / total;
    };
    const double soft = 20.0 * kDeg2Rad, R = soft * 0.5;
    double worst45 = 0.0, worst15 = 0.0;
    for (double f = -0.9; f <= 0.9; f += 0.1) {
        worst45 = std::max(worst45, std::abs(edgeProfile(f * R, soft) - exactly(45.0 * kDeg2Rad, soft, f * R)));
        worst15 = std::max(worst15, std::abs(edgeProfile(f * R, soft) - exactly(15.0 * kDeg2Rad, soft, f * R)));
    }
    CHECK(worst45 < 0.02);     // a 45-degree spot blurred by 20: the flat case is close
    CHECK(worst15 < 0.08);     // a 15-degree one: the blur is comparable to the shape
    CHECK(worst15 > worst45);  // and the error grows with curvature, which is the point
}

TEST_CASE("near the pole a sector is narrower than its own fade, and never reaches 1") {
    //  Not a defect, and worth pinning because it looks like one: a sector's width in arc closes as
    //  sin(theta), so close enough to the axis the fade is wider than the sector and its centre is
    //  already on the ramp. At 70 degrees of elevation a quarter-sector is 7.7 degrees of arc from
    //  centre to edge, against a 10-degree half-fade.
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 4;
    s.fill = 0.5;
    s.softness = 20.0 * kDeg2Rad;
    CHECK(valueAt(s, fromAzEl(0.0, 0.0)) == 1.0);                      // wide at the equator
    const double atPole = valueAt(s, fromAzEl(0.0, 70.0 * kDeg2Rad));  // its own centre, up high
    CHECK(atPole < 1.0);
    CHECK(atPole > 0.5);
}

TEST_CASE("dot sets are the regular solids") {
    //  The counts, that every direction is a unit vector, and that the set is antipodally symmetric
    //  -- which every one of these solids is, and which a transcription error in a golden-ratio
    //  triple would break.
    for (const int n : {2, 4, 6, 8, 12, 20}) {
        const auto d = dotDirections(n);
        REQUIRE(static_cast<int>(d.size()) == n);
        for (const Vec3& v : d) CHECK(length(v) == doctest::Approx(1.0).epsilon(1e-12));
        if (n == 4) continue;  // the tetrahedron is the one that is not
        for (const Vec3& v : d) {
            bool hasOpposite = false;
            for (const Vec3& w : d) hasOpposite = hasOpposite || sameDir(w, v * -1.0, 1e-12);
            CHECK(hasOpposite);
        }
    }
    CHECK(dotDirections(5).empty());
    CHECK(dotDirections(0).empty());
}

TEST_CASE("the largest dot size is half the smallest angle in the set") {
    //  Against the closed forms, which are independent of how the sets are built: a tetrahedron's
    //  vertices subtend acos(-1/3), a cube's nearest pair acos(1/3), an octahedron's a right angle.
    //  Catches: a solid built wrong, where the spacing would be plausible but not exact.
    CHECK(maxDotSize(2) == doctest::Approx(kPi * 0.5));
    CHECK(maxDotSize(4) == doctest::Approx(std::acos(-1.0 / 3.0) * 0.5));
    CHECK(maxDotSize(6) == doctest::Approx(kPi * 0.25));
    CHECK(maxDotSize(8) == doctest::Approx(std::acos(1.0 / 3.0) * 0.5));
    CHECK(maxDotSize(12) * kRad2Deg == doctest::Approx(31.717).epsilon(1e-4));
    CHECK(maxDotSize(20) * kRad2Deg == doctest::Approx(20.905).epsilon(1e-4));
    CHECK(maxDotSize(7) == 0.0);
}

TEST_CASE("a dot region is 1 at every dot and 0 between them") {
    Region r;
    r.kind = RegionKind::Dots;
    r.dots = 6;
    r.softness = 0.0;
    r.dotSize = 20.0 * kDeg2Rad;
    for (const Vec3& v : dotDirections(6)) CHECK(valueAt(r, v) == 1.0);
    //  the middle of three mutually adjacent dots is as far from all of them as a point can be
    CHECK(valueAt(r, unit(Vec3{1, 1, 1})) == 0.0);
    //  at the largest size that does not overlap, neighbours still just touch rather than merge
    r.dotSize = maxDotSize(6);
    CHECK(valueAt(r, unit(Vec3{1, 1, 0})) == 1.0);
}

TEST_CASE("an unsupported dot count is empty, not everything") {
    //  A shape that cannot be built must read as nowhere rather than as everywhere: a fallback of
    //  "inside" would silently pass a whole field.
    Region r;
    r.kind = RegionKind::Dots;
    r.dots = 7;
    r.softness = 0.0;
    for (const Vec3& d : everywhere()) CHECK(valueAt(r, d) == 0.0);
    Region s;
    s.kind = RegionKind::Sectors;
    s.sectors = 0;
    s.softness = 0.0;
    for (const Vec3& d : everywhere()) CHECK(valueAt(s, d) == 0.0);
}

TEST_CASE("a setting that is not a number never reaches the value") {
    //  Catches: the guard removed, letting a NaN softness through edgeProfile's acos, which a
    //  smoother downstream would then hold until its next reset.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const RegionKind k : {RegionKind::Spot, RegionKind::Band, RegionKind::Sectors, RegionKind::Dots})
        for (const double bad : {nan, inf, -inf}) {
            for (int field = 0; field < 6; ++field) {
                Region r;
                r.kind = k;
                r.softness = 0.3;
                (field == 0   ? r.softness
                 : field == 1 ? r.size
                 : field == 2 ? r.yaw
                 : field == 3 ? r.thickness
                 : field == 4 ? r.fill
                              : r.dotSize) = bad;
                for (const Vec3& d : everywhere(40)) {
                    const double v = valueAt(r, d);
                    CHECK(std::isfinite(v));
                    CHECK(v >= 0.0);
                    CHECK(v <= 1.0);
                }
            }
        }
}

TEST_CASE("settings in degrees become a region in radians, every one of them, with the turn added") {
    //  Each setting gets a value no other has, so one that skipped the conversion, or landed in its
    //  neighbour's field, is off by a factor nothing else explains.
    RegionSettingsDeg in;
    in.yaw = 10, in.pitch = 20, in.roll = 30, in.softness = 40, in.size = 50;
    in.bandElevation = 60, in.thickness = 70, in.fill = 0.8, in.dotSize = 9;
    const RegionShape shape{RegionKind::Dots, 5, 12};
    const Region r = resolveRegion(shape, RegionSide::Outside, in, 0.1, 0.2, 0.3);
    CHECK(r.kind == RegionKind::Dots);
    CHECK(r.side == RegionSide::Outside);
    CHECK(r.sectors == 5);
    CHECK(r.dots == 12);
    CHECK(std::abs(r.yaw - (10 * kDeg2Rad + 0.1)) < 1e-12);
    CHECK(std::abs(r.pitch - (20 * kDeg2Rad + 0.2)) < 1e-12);
    CHECK(std::abs(r.roll - (30 * kDeg2Rad + 0.3)) < 1e-12);
    CHECK(std::abs(r.softness - 40 * kDeg2Rad) < 1e-12);
    CHECK(std::abs(r.size - 50 * kDeg2Rad) < 1e-12);
    CHECK(std::abs(r.bandElevation - 60 * kDeg2Rad) < 1e-12);
    CHECK(std::abs(r.thickness - 70 * kDeg2Rad) < 1e-12);
    CHECK(r.fill == 0.8);
    CHECK(std::abs(r.dotSize - 9 * kDeg2Rad) < 1e-12);
}

TEST_CASE("a region that cannot be evaluated is made into one that can") {
    //  seven dots is no solid, and would read 0 everywhere
    CHECK(sanitised({RegionKind::Dots, 4, 7}).dots == 6);
    CHECK(sanitised({RegionKind::Sectors, 0, 6}).sectors == 1);
    CHECK(sanitised({RegionKind::Sectors, 900, 6}).sectors == kMaxSectors);
    for (const int n : {2, 4, 6, 8, 12, 20}) CHECK(sanitised({RegionKind::Dots, 4, n}).dots == n);

    //  dots any larger than this touch, and the set stops being dots
    RegionSettingsDeg big;
    big.dotSize = 80;
    const Region d = resolveRegion({RegionKind::Dots, 4, 20}, RegionSide::Inside, big);
    CHECK(d.dotSize == maxDotSize(20));

    //  a setting that is not a number takes its default, and a turn that is not one is no turn
    const double nan = std::numeric_limits<double>::quiet_NaN();
    RegionSettingsDeg bad;
    bad.softness = nan, bad.size = nan, bad.yaw = nan, bad.fill = nan;
    const Region r = resolveRegion({RegionKind::Spot, 4, 6}, RegionSide::Inside, bad, nan, 0.0, 0.0);
    const RegionSettingsDeg def;
    CHECK(r.softness == def.softness * kDeg2Rad);
    CHECK(r.size == def.size * kDeg2Rad);
    CHECK(r.yaw == 0.0);
    CHECK(r.fill == def.fill);
}

TEST_CASE("the aim on a flat map keeps its azimuth at a pole and over the top") {
    //  Catches: the azimuth read off the direction (undefined at a pole), the flip over the top
    //  forgotten, and the upright kinds' tilt lost.
    Region spot;
    spot.kind = RegionKind::Spot;
    spot.yaw = 40.0 * kDeg2Rad;
    spot.pitch = 30.0 * kDeg2Rad;
    auto m = aimOnMap(spot);
    CHECK(!m.polar);
    CHECK(m.azimuth == doctest::Approx(azimuth(aimOf(spot))));
    CHECK(m.elevation == doctest::Approx(elevation(aimOf(spot))));

    spot.pitch = 90.0 * kDeg2Rad;  // straight up: the direction has no azimuth, the angles do
    m = aimOnMap(spot);
    CHECK(m.polar);
    CHECK(m.azimuth == doctest::Approx(40.0 * kDeg2Rad));

    spot.pitch = 100.0 * kDeg2Rad;  // over the top: it now faces the other way
    m = aimOnMap(spot);
    CHECK(!m.polar);
    CHECK(m.azimuth == doctest::Approx(-140.0 * kDeg2Rad));
    CHECK(m.elevation == doctest::Approx(80.0 * kDeg2Rad));

    Region band;  // sits upright: its aim is up at pitch 0
    band.kind = RegionKind::Band;
    band.yaw = -25.0 * kDeg2Rad;
    m = aimOnMap(band);
    CHECK(m.polar);
    CHECK(m.azimuth == doctest::Approx(-25.0 * kDeg2Rad));
}
