// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstring>
#include <vector>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/region/projection.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

Region softSpot() {
    Region r;
    r.kind = RegionKind::Spot;
    r.size = 60.0 * kDeg2Rad;
    r.softness = 40.0 * kDeg2Rad;
    r.yaw = 30.0 * kDeg2Rad;
    r.pitch = 20.0 * kDeg2Rad;
    return r;
}

//  The operator as a matrix, by putting one channel through it at a time.
std::vector<double> matrixOf(RegionOperator& op, int order) {
    const int C = numChannels(order);
    std::vector<double> m(static_cast<std::size_t>(C * C));
    std::vector<float> in(static_cast<std::size_t>(C)), out(in.size());
    for (int j = 0; j < C; ++j) {
        std::fill(in.begin(), in.end(), 0.0f);
        in[static_cast<std::size_t>(j)] = 1.0f;
        op.apply(in.data(), out.data(), 1);
        for (int i = 0; i < C; ++i) m[static_cast<std::size_t>(i * C + j)] = out[static_cast<std::size_t>(i)];
    }
    return m;
}

/*  The definition, integrated the slow way: valueAt on a grid over the whole sphere, at the region's
 *  real orientation. Nothing here knows about poles, blocks or rotations. */
std::vector<double> reference(const Region& r, int order) {
    const int C = numChannels(order), nz = 240, na = 480;
    std::vector<double> z, w, y(static_cast<std::size_t>(C)), m(static_cast<std::size_t>(C * C), 0.0);
    gaussLegendre(nz, z, w);
    for (int a = 0; a < nz; ++a)
        for (int b = 0; b < na; ++b) {
            const double az = 2.0 * kPi * (b + 0.5) / na, s = std::sqrt(1.0 - z[a] * z[a]);
            const Vec3 d{s * std::cos(az), s * std::sin(az), z[a]};
            const double g = valueAt(r, d) * w[a] * (2.0 * kPi / na) / (4.0 * kPi);
            if (g == 0.0) continue;
            shSN3D(d, order, y);
            for (int i = 0; i < C; ++i)
                for (int j = 0; j < C; ++j)
                    m[static_cast<std::size_t>(i * C + j)] += g * y[i] * y[j] * (2.0 * acnOrder(j) + 1.0);
        }
    return m;
}

}  // namespace

TEST_CASE("a region on the bus is the integral of what it is worth in every direction, wherever it points") {
    /*  The operator -- turn to the pole, one block per m from a 1-D integral, turn back -- against
        the definition integrated over the whole sphere with valueAt, for a soft spot and a soft
        band at an orientation that is nothing special.
        Catches: the (2n+1) on the wrong row, a turn the wrong way round, a block transposed, and
        the natural axis forgotten -- a spot faces front at zero and a band stands up. */
    const int order = 3, C = 16;
    Region band;
    band.kind = RegionKind::Band;
    band.bandElevation = 25.0 * kDeg2Rad;
    band.thickness = 50.0 * kDeg2Rad;
    band.softness = 30.0 * kDeg2Rad;
    band.yaw = -70.0 * kDeg2Rad;
    band.pitch = 35.0 * kDeg2Rad;
    band.roll = 0.8;
    for (const Region& r : {softSpot(), band}) {
        RegionOperator op;
        op.prepare(order, 8);
        REQUIRE(op.set(r));
        const auto got = matrixOf(op, order), want = reference(r, order);
        double worst = 0.0;
        for (int k = 0; k < C * C; ++k)
            worst = std::max(worst, std::abs(got[static_cast<std::size_t>(k)] - want[static_cast<std::size_t>(k)]));
        INFO("worst entry ", worst);
        CHECK(worst < 2e-4);
    }
}

TEST_CASE("a hard cap's omni entry is the share of the sphere it covers, and the bus rings outside 0..1") {
    //  Closed forms. The first entry of M is the region's average, which for a cap is (1 - cos)/2.
    const int order = 3;
    Region cap;
    cap.kind = RegionKind::Spot;
    cap.softness = 0.0;
    RegionOperator op;
    op.prepare(order, 8);
    for (const double deg : {30.0, 90.0, 140.0}) {
        cap.size = deg * kDeg2Rad;
        REQUIRE(op.set(cap));
        CHECK(std::abs(op.blocks()[0] - (1.0 - std::cos(cap.size)) / 2.0) < 1e-12);
    }

    //  and a hard band's is the share between its two latitudes. Exact only if the integral is cut AT
    //  the band's edges: a step inside a stretch is off by a thousandth, which is what this catches.
    Region band;
    band.kind = RegionKind::Band;
    band.softness = 0.0;
    band.bandElevation = 25.0 * kDeg2Rad;
    band.thickness = 50.0 * kDeg2Rad;
    REQUIRE(op.set(band));
    CHECK(std::abs(op.blocks()[0] - (std::sin(50.0 * kDeg2Rad) - std::sin(0.0)) / 2.0) < 1e-12);

    //  "front" at order 3 -- a half -- reads about -0.2 to 1.2 on the bus: the first row of M is the
    //  region itself, band-limited. Its truncation, not a defect.
    cap.size = kPi / 2;
    REQUIRE(op.set(cap));
    const auto m = matrixOf(op, order);
    std::vector<double> y(16);
    double lo = 1e9, hi = -1e9;
    for (int a = 0; a < 360; ++a) {
        shSN3D(fromAzEl(a * kDeg2Rad, 0.0), order, y);
        double g = 0.0;
        for (int j = 0; j < 16; ++j) g += m[static_cast<std::size_t>(j)] * y[static_cast<std::size_t>(j)];
        lo = std::min(lo, g);
        hi = std::max(hi, g);
    }
    CHECK(lo == doctest::Approx(-0.2).epsilon(0.1));  // values near 1: Approx's tolerance is what it says
    CHECK(hi == doctest::Approx(1.2).epsilon(0.05));
}

TEST_CASE("inside and outside add up to the field, exactly as far as floats go, and everywhere passes it all") {
    const int order = 5, C = numChannels(order), frames = 6;
    std::vector<float> in(static_cast<std::size_t>(C * frames)), a(in.size()), b(in.size());
    unsigned s = 9u;
    for (float& v : in) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(s >> 8) % 2001 - 1000) / 1000.0f;
    }

    Region r = softSpot();
    RegionOperator inside, outside;
    inside.prepare(order, frames);
    outside.prepare(order, frames);
    REQUIRE(inside.set(r));
    r.side = RegionSide::Outside;
    REQUIRE(outside.set(r));
    inside.apply(in.data(), a.data(), frames);
    outside.apply(in.data(), b.data(), frames);
    double moved = 0.0;
    for (std::size_t k = 0; k < in.size(); ++k) {
        CHECK(std::abs(a[k] + b[k] - in[k]) < 1e-6);
        moved = std::max(moved, static_cast<double>(std::abs(a[k] - in[k])));
    }
    CHECK(moved > 0.1);

    Region all;  // Everywhere
    REQUIRE(inside.set(all));
    inside.apply(in.data(), a.data(), frames);
    CHECK(std::memcmp(a.data(), in.data(), in.size() * sizeof(float)) == 0);
    all.side = RegionSide::Outside;
    REQUIRE(inside.set(all));
    inside.apply(in.data(), a.data(), frames);
    for (const float v : a) CHECK(v == 0.0f);
}

TEST_CASE("sectors and dots are projected too, and agree with the definition") {
    /*  Neither is axial -- a sector varies with azimuth, dots are a scatter of spots -- so each is
        the full matrix, built by a 2-D quadrature whose integrand is valueAt. Checked against the
        same slow reference the axial kinds are: valueAt on a 240 x 480 grid, at an orientation that
        is nothing special.

        Catches: the (2n+1) moved to the row, the 1/4pi dropped, the azimuthal pair table indexed by
        |m| rather than by signed m (which makes every sine term a cosine), and the radial part read
        from the wrong ACN entry for a negative m. */
    for (const int order : {1, 3}) {
        Region sectors;
        sectors.kind = RegionKind::Sectors;
        sectors.sectors = 3;
        sectors.fill = 0.4;
        sectors.softness = 25.0 * kDeg2Rad;
        sectors.yaw = 25.0 * kDeg2Rad;
        sectors.pitch = -15.0 * kDeg2Rad;
        sectors.roll = 40.0 * kDeg2Rad;

        Region dots;
        dots.kind = RegionKind::Dots;
        dots.dots = 6;
        dots.dotSize = 28.0 * kDeg2Rad;
        dots.softness = 15.0 * kDeg2Rad;
        dots.yaw = -35.0 * kDeg2Rad;
        dots.pitch = 20.0 * kDeg2Rad;

        //  every dot set has cells of its own shape, and the tetrahedron is the one without opposite
        //  pairs; a fade wide enough to reach past a cell's edge is where the cell has to be cut
        Region tetra = dots, twenty = dots, wide = dots, one = sectors;
        tetra.dots = 4, tetra.dotSize = 35.0 * kDeg2Rad;
        twenty.dots = 20, twenty.dotSize = 15.0 * kDeg2Rad, twenty.softness = 30.0 * kDeg2Rad;
        wide.softness = 150.0 * kDeg2Rad;
        one.sectors = 1, one.fill = 0.7, one.softness = 120.0 * kDeg2Rad;

        for (const Region& r : {sectors, dots, tetra, twenty, wide, one}) {
            RegionOperator op;
            op.prepare(order, 4);
            REQUIRE(op.set(r));  // built in the step itself
            CHECK_FALSE(op.isAxial());
            const std::vector<double> got = matrixOf(op, order), want = reference(r, order);
            double worst = 0.0;
            for (std::size_t i = 0; i < want.size(); ++i) worst = std::max(worst, std::abs(got[i] - want[i]));
            INFO("order ", order, ", kind ", static_cast<int>(r.kind), ", worst entry off by ", worst);
            CHECK(worst < 5.0e-4);
        }
    }
}

TEST_CASE("turning a sector rebuilds nothing; resizing it rebuilds the matrix") {
    /*  What makes a dense kind affordable: the matrix depends on the shape alone, and orientation --
        which is what a modulated region moves -- costs only the rotation an axial region also pays.
        Catches: caching on the whole region (every turn rebuilds) and caching on nothing (the first
        check would pass for the wrong reason, which is why the second is here). */
    Region r;
    r.kind = RegionKind::Sectors;
    r.sectors = 4;
    r.fill = 0.5;
    r.softness = 20.0 * kDeg2Rad;

    RegionOperator op;
    op.prepare(3, 4);
    REQUIRE(op.set(r));
    const std::vector<double> first(op.dense().begin(), op.dense().end());

    //  turned: the own-frame matrix is the same to the bit, and the rotation carries the change
    r.yaw = 40.0 * kDeg2Rad;
    r.pitch = 15.0 * kDeg2Rad;
    REQUIRE(op.set(r));
    const std::vector<double> turned(op.dense().begin(), op.dense().end());
    REQUIRE(turned.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(turned[i] == first[i]);

    //  and the side is not a shape either: outside is I - M, never a second matrix
    r.side = RegionSide::Outside;
    REQUIRE(op.set(r));
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(op.dense()[i] == first[i]);

    //  resized: it is a different matrix
    r.side = RegionSide::Inside;
    r.fill = 0.25;
    REQUIRE(op.set(r));
    bool moved = false;
    for (std::size_t i = 0; i < first.size(); ++i) moved = moved || op.dense()[i] != first[i];
    CHECK(moved);
}

TEST_CASE("the outside of everywhere passes nothing, and says so") {
    /*  Catches: passesEverything() reading the identity flag alone, which would leave the field
     *  whole for a caller that skips apply() on its word, where the region asks for silence. */
    const int order = 3, C = 16;
    Region all;
    all.kind = RegionKind::Everywhere;
    RegionOperator op;
    op.prepare(order, 4);
    REQUIRE(op.set(all));
    CHECK(op.passesEverything());

    all.side = RegionSide::Outside;
    REQUIRE(op.set(all));
    CHECK_FALSE(op.passesEverything());
    std::vector<float> in(static_cast<std::size_t>(C), 0.5f), out(in.size(), 1.0f);
    op.apply(in.data(), out.data(), 1);
    for (const float v : out) CHECK(v == 0.0f);
}

TEST_CASE("a region gives the same bits for one call of many frames as for many calls of one") {
    const int order = 7, C = numChannels(order), frames = 40;
    std::vector<float> in(static_cast<std::size_t>(C * frames));
    unsigned s = 2u;
    for (float& v : in) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(s >> 8) % 2001 - 1000) / 1000.0f;
    }
    RegionOperator op;
    op.prepare(order, frames);
    REQUIRE(op.set(softSpot()));
    std::vector<float> whole(in.size()), ones(in.size());
    op.apply(in.data(), whole.data(), frames);
    for (int f = 0; f < frames; ++f) op.apply(in.data() + f * C, ones.data() + f * C, 1);
    CHECK(std::memcmp(whole.data(), ones.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE(
    "the dense kinds hold at orders 7 and 10: a sector that is everything is the identity, and hard dots cover their "
    "caps") {
    /*  Two answers known exactly, at the top order the bus runs at, where a rule too coarse for
     *  order-14 harmonics shows first. Order 10 is the most an effect is prepared at (kMaxHostOrder).
     *  Catches: stretches and fades in one panel each (the identity is 0.6 off), and dots summed
     *  without their cells or turned by the wrong symmetry (the covered share moves). */
    for (const int order : {7, 10}) {
        const int C = numChannels(order);
        Region all;
        all.kind = RegionKind::Sectors;
        all.sectors = 3;
        all.fill = 1.0;
        all.softness = 0.0;
        all.yaw = 0.4, all.pitch = -0.3;
        RegionOperator op;
        op.prepare(order, 4);
        REQUIRE(op.set(all));
        const auto m = matrixOf(op, order);
        double worst = 0.0;
        for (int i = 0; i < C; ++i)
            for (int j = 0; j < C; ++j)
                worst = std::max(worst, std::abs(m[static_cast<std::size_t>(i * C + j)] - (i == j ? 1.0 : 0.0)));
        INFO("worst entry off the identity ", worst);
        CHECK(worst < 1e-4);

        for (const int count : {4, 6, 8, 12, 20}) {
            Region dots;
            dots.kind = RegionKind::Dots;
            dots.dots = count;
            dots.dotSize = maxDotSize(count);  // as large as they go: the caps just touch
            dots.softness = 0.0;
            REQUIRE(op.set(dots));
            //  W in, W out: the share of the sphere the caps cover
            const double share = count * (1.0 - std::cos(dots.dotSize)) * 0.5;
            INFO(count, " dots");
            CHECK(op.dense()[0] == doctest::Approx(share).epsilon(1e-4));
            //  and a set is the same from every one of its dots: its matrix has the set's symmetry, so the
            //  first-order block is a multiple of the identity for every set but four's, which is too
            if (count != 2) {
                for (int i = 1; i < 4; ++i)
                    for (int j = 1; j < 4; ++j)
                        CHECK(op.dense()[static_cast<std::size_t>(i * C + j)] ==
                              doctest::Approx(i == j ? op.dense()[static_cast<std::size_t>(C + 1)] : 0.0)
                                  .epsilon(1e-6)
                                  .scale(1.0));
            }
        }
    }
}

TEST_CASE("half the sphere as one sector and the half opposite add up to everything, however wide the fade") {
    /*  One sector at fill 0.5 fades symmetrically about its edge, and a profile and its mirror add to one,
     *  so it and itself turned half round are the whole sphere: M + M' = I, exactly, at any softness. At
     *  order 7 with a fade across half the circle, a fade rule of one panel is visibly short of it. */
    for (const int order : {7, 10}) {
        const int C = numChannels(order);
        Region half;
        half.kind = RegionKind::Sectors;
        half.sectors = 1;
        half.fill = 0.5;
        half.softness = 180.0 * kDeg2Rad;
        Region opposite = half;
        opposite.yaw = kPi;
        RegionOperator a, b;
        a.prepare(order, 4);
        b.prepare(order, 4);
        REQUIRE(a.set(half));
        REQUIRE(b.set(opposite));
        const auto ma = matrixOf(a, order), mb = matrixOf(b, order);
        double worst = 0.0;
        for (int i = 0; i < C; ++i)
            for (int j = 0; j < C; ++j) {
                const auto k = static_cast<std::size_t>(i * C + j);
                worst = std::max(worst, std::abs(ma[k] + mb[k] - (i == j ? 1.0 : 0.0)));
            }
        INFO("order ", order, ", worst entry off the identity ", worst);
        CHECK(worst < 1e-4);
    }
}
