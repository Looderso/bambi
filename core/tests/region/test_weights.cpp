// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <vector>

#include "bambi/encode/params.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/region/projection.hpp"
#include "bambi/region/shape.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
Region clouds(int seed, double coverage, double contrast, double detail, double evolveDeg) {
    Region r;
    r.kind = RegionKind::Clouds;
    r.seed = seed;
    r.coverage = coverage;
    r.contrast = contrast;
    r.detail = detail;
    r.evolve = evolveDeg * kDeg2Rad;
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
}  // namespace

TEST_CASE(
    "clouds: the mean is the coverage and the spread is exactly half the contrast, at every evolve, order and seed "
    "(REGIONS 2)") {
    /*  Under SN3D an order's weights w add |w|^2 / (2n+1) to the field's variance.
        Catches: dropping the per-order scaling of the basis, the orthogonalisation of B against A,
        or the scale k -- any of which makes the spread breathe with evolve or miss what was asked. */
    for (const int order : {1, 3, 7})
        for (const int seed : {1, 7, 42})
            for (const double detail : {0.0, 0.4, 1.0})
                for (const double evolve : {0.0, 33.0, 90.0, 200.0, 359.0}) {
                    std::array<double, kRegionWeights> w{};
                    regionWeights(clouds(seed, 0.3, 0.6, detail, evolve), order, w);
                    CHECK(w[0] == doctest::Approx(0.3));
                    double var = 0.0;
                    for (int i = 1; i < numChannels(order); ++i)
                        var +=
                            w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)] / (2.0 * acnOrder(i) + 1.0);
                    INFO("order ", order, " seed ", seed, " detail ", detail, " evolve ", evolve);
                    CHECK(std::sqrt(var) == doctest::Approx(0.3).epsilon(1e-9));
                    for (int i = numChannels(order); i < kRegionWeights; ++i)
                        CHECK(w[static_cast<std::size_t>(i)] == 0.0);
                }
    //  and the variance claim itself, once, on the sphere: the exact field's spread is what the weights say
    const Region r = clouds(7, 0.5, 0.8, 0.5, 40.0);
    const RegionField f(r);
    std::vector<double> z, wz;
    gaussLegendre(120, z, wz);
    double mean = 0.0, sq = 0.0;
    const int na = 360;
    for (std::size_t a = 0; a < z.size(); ++a)
        for (int b = 0; b < na; ++b) {
            const double az = 2.0 * kPi * (b + 0.5) / na, s = std::sqrt(1.0 - z[a] * z[a]);
            const double g = f.at({s * std::cos(az), s * std::sin(az), z[a]}), weight = wz[a] / na / 2.0;
            mean += g * weight;
            sq += g * g * weight;
        }
    CHECK(mean == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(std::sqrt(sq - mean * mean) == doctest::Approx(0.4).epsilon(1e-6));
}

TEST_CASE("clouds: evolve loops at a whole turn, and a different seed is a different pattern") {
    std::array<double, kRegionWeights> a{}, b{}, c{};
    regionWeights(clouds(3, 0.5, 0.6, 0.5, 0.0), 3, a);
    regionWeights(clouds(3, 0.5, 0.6, 0.5, 360.0), 3, b);
    regionWeights(clouds(4, 0.5, 0.6, 0.5, 0.0), 3, c);
    double same = 0.0, other = 0.0;
    for (int i = 0; i < 16; ++i) {
        same = std::max(same, std::abs(a[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)]));
        other = std::max(other, std::abs(a[static_cast<std::size_t>(i)] - c[static_cast<std::size_t>(i)]));
    }
    CHECK(same < 1e-12);
    CHECK(other > 0.05);
}

TEST_CASE("custom: projecting a band-limited field gives its weights back, and everywhere is the omni alone") {
    /*  The rule is exact for a polynomial of the order it is built for.
        Catches: the (2n+1)/4pi on the wrong side, a phi weight dropped, and the field read in the
        world frame instead of its own. */
    Region r;
    r.kind = RegionKind::Custom;
    r.yaw = 0.7, r.pitch = -0.4, r.roll = 1.1;  // orientation is not part of the weights
    for (int i = 0; i < 16; ++i) r.weights[static_cast<std::size_t>(i)] = std::sin(1.7 * i + 0.3) * 0.5;
    std::array<double, kRegionWeights> back{};
    projectWeights(r, 3, back);
    for (int i = 0; i < 16; ++i)
        CHECK(back[static_cast<std::size_t>(i)] ==
              doctest::Approx(r.weights[static_cast<std::size_t>(i)]).epsilon(1e-9));
    for (int i = 16; i < kRegionWeights; ++i) CHECK(back[static_cast<std::size_t>(i)] == 0.0);

    Region all;
    all.kind = RegionKind::Everywhere;
    projectWeights(all, 3, back);
    CHECK(back[0] == doctest::Approx(1.0).epsilon(1e-9));
    for (int i = 1; i < 16; ++i) CHECK(std::abs(back[static_cast<std::size_t>(i)]) < 1e-9);

    //  and a spot's omni weight is the share of the sphere it covers, as the operator's is
    Region spot;
    spot.kind = RegionKind::Spot;
    spot.size = 60.0 * kDeg2Rad;
    projectWeights(spot, 1, back);
    CHECK(back[0] == doctest::Approx((1.0 - std::cos(spot.size)) * 0.5).epsilon(1e-3));
}

TEST_CASE("custom on the bus: the matrix of its weights, and a weights kind is what valueAt says it is") {
    Region r;
    r.kind = RegionKind::Custom;
    r.weights[0] = 0.6;
    r.weights[2] = 0.3;  // Z: brighter up
    r.weights[5] = -0.2;
    r.yaw = 0.4;
    RegionOperator op;
    op.prepare(3, 8);
    REQUIRE(op.set(r));
    CHECK_FALSE(op.passesEverything());  // built in the step itself
    //  W in, W out is the omni row of M: the field's mean plus what the up-weight folds into W
    std::vector<float> in(16, 0.0f), out(16, 0.0f);
    in[0] = 1.0f;
    op.apply(in.data(), out.data(), 1);
    CHECK(out[0] == doctest::Approx(0.6).epsilon(1e-4));
    //  valueAt, in the region's own frame, is the weights times the harmonics
    const Vec3 up{0, 0, 1};
    std::array<double, kRegionWeights> y{};
    Region own = r;
    own.yaw = own.pitch = own.roll = 0.0;
    shSN3D(up, 7, y);
    double want = 0.0;
    for (int i = 0; i < kRegionWeights; ++i)
        want += own.weights[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(i)];
    CHECK(valueAt(own, up) == doctest::Approx(want));
    own.side = RegionSide::Outside;
    CHECK(valueAt(own, up) == doctest::Approx(1.0 - want));
}

TEST_CASE("clouds on the bus are the matrix of their weights, and every setting moves it in the step") {
    /*  Clouds and custom share one path: their weights are their harmonics, checked here by
        comparing against custom fed the same weights.
        Catches: a wrong gain, the sine term dropped, or the coverage off the omni weight. */
    const int order = 3, C = 16;
    Region c = clouds(11, 0.4, 0.9, 0.3, 25.0);
    RegionOperator op;
    op.prepare(order, 8);
    REQUIRE(op.set(c));
    const auto got = matrixOf(op, order);

    Region asCustom = c;
    asCustom.kind = RegionKind::Custom;
    regionWeights(c, order, asCustom.weights);
    RegionOperator direct;
    direct.prepare(order, 8);
    REQUIRE(direct.set(asCustom));
    const auto want = matrixOf(direct, order);
    double worst = 0.0;
    for (int k = 0; k < C * C; ++k)
        worst = std::max(worst, std::abs(got[static_cast<std::size_t>(k)] - want[static_cast<std::size_t>(k)]));
    INFO("worst entry ", worst);
    CHECK(worst < 1e-5);

    //  every setting moves the matrix, and so does a new seed
    for (Region moved :
         {clouds(11, 0.7, 0.9, 0.3, 25.0), clouds(11, 0.4, 0.2, 0.3, 25.0), clouds(11, 0.4, 0.9, 0.9, 25.0),
          clouds(11, 0.4, 0.9, 0.3, 140.0), clouds(12, 0.4, 0.9, 0.3, 25.0)}) {
        REQUIRE(op.set(moved));
        const auto now = matrixOf(op, order);
        double diff = 0.0;
        for (int k = 0; k < C * C; ++k)
            diff = std::max(diff, std::abs(now[static_cast<std::size_t>(k)] - got[static_cast<std::size_t>(k)]));
        CHECK(diff > 1e-4);
    }
}

TEST_CASE("custom plays each weight times its order's gain, folded in at resolve") {
    //  Catches: dropping the multiply in resolveRegion (the region ignores the gains) and indexing
    //  the gain by channel instead of order (order 2's three cells would part).
    RegionShape shape;
    shape.custom = true;
    for (int i = 0; i < 16; ++i) shape.weights[static_cast<std::size_t>(i)] = 0.1 * (i + 1);
    shape.gains = {1.0, 0.5, 2.0, 0.0, 1.0, 1.0, 1.0, 1.0};
    const Region r = resolveRegion(shape, RegionSide::Inside, {});
    CHECK(r.kind == RegionKind::Custom);
    CHECK(r.weights[0] == doctest::Approx(0.1));
    CHECK(r.weights[2] == doctest::Approx(0.3 * 0.5));
    CHECK(r.weights[6] == doctest::Approx(0.7 * 2.0));
    CHECK(r.weights[12] == doctest::Approx(0.0));
    //  a gain that is not a number, or out of range, is made one that can be played
    shape.gains[1] = 9.0;
    shape.gains[2] = std::nan("");
    CHECK(sanitised(shape).gains[1] == doctest::Approx(2.0));
    CHECK(sanitised(shape).gains[2] == doctest::Approx(1.0));
}

TEST_CASE("a custom region's weights and a cloud's seed are state, and round-trip by name") {
    //  Catches: dropping the write or the read of either, so the region comes back a fresh spot.
    const ParamManifest& m = encodeParams();
    PluginState out{m};
    out.regions[0].shape.kind = RegionKind::Band;  // kept under the custom
    out.regions[0].shape.custom = true;
    out.regions[0].shape.seed = 23;
    out.regions[0].shape.gains[3] = 0.25;  // and a gain per order rides with the weights
    for (int i = 0; i < kRegionWeights; ++i) out.regions[0].shape.weights[static_cast<std::size_t>(i)] = 0.01 * i;
    const auto text = saveState(Product::Encoder, m, out);
    CHECK(text.find("\"custom\"") != std::string::npos);
    PluginState back{m};
    REQUIRE(loadState(Product::Encoder, m, text, back).ok);
    CHECK(back.regions[0].shape == out.regions[0].shape);
    //  and resolved, it is a custom region, the band kept for when custom is left
    CHECK(resolveRegion(back.regions[0].shape, RegionSide::Inside, {}).kind == RegionKind::Custom);
    //  a document that names custom as the kind loads as the flag over a spot
    PluginState named{m};
    REQUIRE(loadState(Product::Encoder, m,
                      "{\"product\":\"encoder\",\"version\":1,\"regions\":{\"region1\":{\"kind\":\"custom\"}}}", named)
                .ok);
    CHECK(named.regions[0].shape.custom);
    CHECK(named.regions[0].shape.kind == RegionKind::Spot);

    PluginState cl{m};
    cl.regions[0].shape.kind = RegionKind::Clouds;
    cl.regions[0].shape.seed = 500;  // past the largest: held to it
    const auto ctext = saveState(Product::Encoder, m, cl);
    CHECK(ctext.find("\"weights\"") == std::string::npos);  // only custom carries them
    PluginState cback{m};
    REQUIRE(loadState(Product::Encoder, m, ctext, cback).ok);
    CHECK(cback.regions[0].shape.kind == RegionKind::Clouds);
    CHECK(cback.regions[0].shape.seed == kMaxCloudSeed);
}
