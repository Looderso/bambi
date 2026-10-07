// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/binaural.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/patch/json.hpp"
#include "doctest.h"

using namespace bambi;

/*  The reference is prototypes/decoder/sheets/check-filters.py: the same design in numpy, with numpy's own
 *  eigenvalues, SVD and pseudo-inverse where the core has closed forms. Its filters are in filters-fixture.json. */
namespace {
std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const HeadModel& shipped() {
    static const HeadModel model = [] {
        HeadModel m;
        std::string error;
        REQUIRE_MESSAGE(parseHeadModel(readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json"), m, &error),
                        error);
        return m;
    }();
    return model;
}

const Json& fixture() {
    static const Json f = Json::parse(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/filters-fixture.json"));
    return f;
}

double number(const Json& j, const char* key) { return j.find(key)->numberOr(0); }

FilterDesign designOf(int order) {
    const Json& d = *fixture().find("design");
    FilterDesign f;
    f.order = order;
    f.sampleRate = number(d, "sampleRate");
    f.fftSize = static_cast<int>(number(d, "fftSize"));
    f.taps = static_cast<int>(number(d, "taps"));
    f.delay = static_cast<int>(number(d, "delay"));
    f.fade = static_cast<int>(number(d, "fade"));
    f.headRadius = number(d, "headRadius");
    return f;
}

void fixtureGrid(std::vector<Vec3>& dirs, std::vector<double>& weights) {
    const Json& g = *fixture().find("grid");
    quadratureGrid(static_cast<int>(number(g, "rings")), static_cast<int>(number(g, "azimuths")), dirs, weights);
}
}  // namespace

TEST_CASE("the quadrature grid integrates products of spherical harmonics exactly") {
    //  Catches: weights that do not sum to one, an azimuth step that misses the circle, a ring at the wrong z.
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(12, 20, dirs, w);
    REQUIRE(dirs.size() == 240);
    double sum = 0;
    for (double v : w) sum += v;
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-12));
    const int order = 9, C = numChannels(order);
    std::vector<double> y(static_cast<std::size_t>(C)), gram(static_cast<std::size_t>(C * C), 0.0);
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        shSN3D(dirs[d], order, y);
        for (int i = 0; i < C; ++i)
            for (int j = 0; j < C; ++j)
                gram[static_cast<std::size_t>(i * C + j)] +=
                    w[d] * y[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(j)];
    }
    double worst = 0;
    for (int i = 0; i < C; ++i)
        for (int j = 0; j < C; ++j) {
            const double expect = i == j ? 1.0 / (2.0 * acnOrder(i) + 1.0) : 0.0;  // SN3D: a channel's mean square
            worst = std::max(worst, std::abs(gram[static_cast<std::size_t>(i * C + j)] - expect));
        }
    CHECK(worst < 1e-12);
}

TEST_CASE("the filters equal the reference design tap for tap") {
    //  Catches: a conjugate dropped in the covariance constraint, the interaural delay on the wrong ear, a phase
    //  carried from the wrong bin, the bulk delay's sign, the fade.
    std::vector<Vec3> dirs;
    std::vector<double> w;
    fixtureGrid(dirs, w);
    for (const Json& c : fixture().find("cases")->items()) {
        const int order = static_cast<int>(number(c, "order"));
        BinauralFilterBuilder builder;
        REQUIRE(builder.prepare(shipped(), designOf(order), dirs, w));
        CHECK(builder.cutoffHz() == doctest::Approx(number(c, "cutoffHz")));
        builder.build(number(c, "earShift"), number(c, "headSize"));
        for (const Json& f : c.find("filters")->items()) {
            const std::span<const float> got =
                builder.filter(static_cast<int>(number(f, "acn")), static_cast<int>(number(f, "ear")));
            const std::vector<Json>& taps = f.find("taps")->items();
            REQUIRE(got.size() == taps.size());
            double worst = 0, peak = 0;
            for (std::size_t t = 0; t < taps.size(); ++t) {
                worst = std::max(worst, std::abs(static_cast<double>(got[t]) - taps[t].numberOr(0)));
                peak = std::max(peak, std::abs(taps[t].numberOr(0)));
            }
            CHECK_MESSAGE(worst < 1e-4 * std::max(1.0, peak), "order " << order << " acn " << number(f, "acn")
                                                                       << " ear " << number(f, "ear") << ": " << worst
                                                                       << " off, peak " << peak);
        }
        const std::vector<Json>& bands = c.find("magnitudeError")->items();
        CHECK(builder.magnitudeError(200, 1000) == doctest::Approx(bands[0].numberOr(0)).epsilon(1e-3));
        CHECK(builder.magnitudeError(8000, 16000) == doctest::Approx(bands[3].numberOr(0)).epsilon(1e-3));
    }
}

TEST_CASE("the decode keeps the diffuse field, hears a front source in both ears alike, and ends in time") {
    //  Catches: a constraint that mixes the ears but misses the target by a scale, an asymmetric grid, a filter
    //  whose energy wraps round its end.
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(12, 20, dirs, w);
    for (int order : {1, 3}) {
        BinauralFilterBuilder builder;
        builder.prepare(shipped(), designOf(order), dirs, w);
        builder.build(0.0, 1.0);
        CHECK(builder.diffuseError(200, 16000) < 1e-6);
        CHECK(builder.magnitudeError(8000, 16000) < (order == 1 ? 10.0 : 7.0));  // measured: 9.4 and 6.6 dB

        std::vector<double> y(static_cast<std::size_t>(builder.channels()));
        shSN3D({1, 0, 0}, order, y);
        double same = 0, total = 0, late = 0;
        for (int t = 0; t < builder.taps(); ++t) {
            double left = 0, right = 0;
            for (int c = 0; c < builder.channels(); ++c) {
                left += y[static_cast<std::size_t>(c)] * builder.filter(c, 0)[static_cast<std::size_t>(t)];
                right += y[static_cast<std::size_t>(c)] * builder.filter(c, 1)[static_cast<std::size_t>(t)];
            }
            same += (left - right) * (left - right);
            total += left * left + right * right;
            if (t >= 192) late += left * left + right * right;
        }
        CHECK(same / total < 1e-9);
        CHECK(late / total < 0.01);
    }
}

TEST_CASE("a design that does not hold together is refused, and builds nothing") {
    //  Catches: an FFT size pffft refuses passing silently, so that the filters keep a previous build's values.
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(4, 7, dirs, w);
    FilterDesign bad = designOf(1);
    bad.fftSize = 100;  // not a multiple of 32
    BinauralFilterBuilder builder;
    CHECK_FALSE(builder.prepare(shipped(), bad, dirs, w));
    bad = designOf(1);
    bad.taps = bad.fftSize + 1;
    CHECK_FALSE(builder.prepare(shipped(), bad, dirs, w));
    CHECK(builder.prepare(shipped(), designOf(1), dirs, w));
}

TEST_CASE("the covariance constraint holds on a quiet head too") {
    //  Catches: an absolute lift in the 2x2 roots, which takes over once the ear spectra fall below about -60 dB and
    //  leaves the mix short of the target.
    HeadModel quiet = shipped();
    for (int k = 0; k < quiet.axis.points; ++k)
        quiet.smooth.coef[static_cast<std::size_t>(k)] -= 70.0;  // the order-0 row
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(6, 11, dirs, w);
    BinauralFilterBuilder builder;
    REQUIRE(builder.prepare(quiet, designOf(1), dirs, w));
    builder.build(0.0, 1.0);
    CHECK(builder.diffuseError(200, 16000) < 1e-6);
    CHECK(builder.magnitudeError(1000, 4000) < 5.0);
}
