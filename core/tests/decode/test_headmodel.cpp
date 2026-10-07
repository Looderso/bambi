// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/headmodel.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/patch/json.hpp"
#include "doctest.h"

using namespace bambi;

/*  The head model is learned in the browser (prototypes/decoder/sheets) and shipped as core/data/decode/head.json.
 *  The fixtures beside this test were written by the same page's math (export-fixtures.js) for a few directions and
 *  slider settings: both ears in dB at every axis point and the time difference. The synthesiser must reproduce them.
 */
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

Vec3 direction(double azimuthDeg, double elevationDeg) {
    const double az = azimuthDeg * M_PI / 180.0, el = elevationDeg * M_PI / 180.0;
    return {std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
}

std::vector<double> numbers(const Json& a) {
    std::vector<double> v;
    for (const Json& x : a.items()) v.push_back(x.numberOr(0));
    return v;
}
}  // namespace

TEST_CASE("the shipped model is the one the listening page plays") {
    //  Catches: a loader that reads a wrong key, swaps order and points, or a model file the page never heard.
    const HeadModel& m = shipped();
    CHECK(m.learnedFrom.find("HUTUBS") != std::string::npos);
    CHECK(m.axis.points == 339);
    CHECK(m.axis.step == doctest::Approx(0.1));
    CHECK(m.smooth.order == 4);
    CHECK(m.ild.order == 10);
    CHECK(m.ildWithSize.order == 10);
    CHECK(m.itd.order == 8);
    CHECK(m.notches.order == 6);
    CHECK(m.notches.list.size() == 3);
    CHECK(m.notches.depthFactor == doctest::Approx(2.5));
}

TEST_CASE("both ears and the time difference equal the page's math in every fixture") {
    //  Catches: the ILD applied with the wrong sign or to the wrong ear, a notch without its fade or depth factor, a
    //  centre that leaves the band, a time difference that ignores the head's size.
    const Json fixtures = Json::parse(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/fixtures.json"));
    const Json* cases = fixtures.find("cases");
    REQUIRE(cases != nullptr);
    REQUIRE(cases->size() == 21);

    std::vector<Vec3> dirs;
    for (const Json& c : cases->items())
        dirs.push_back(direction(c.find("azimuth")->numberOr(0), c.find("elevation")->numberOr(0)));
    Head head;
    head.prepare(shipped(), dirs);
    REQUIRE(head.points() == 339);

    int d = 0;
    for (const Json& c : cases->items()) {
        head.synthesise(c.find("earShift")->numberOr(0), c.find("headSize")->numberOr(1));
        const std::vector<double> left = numbers(*c.find("left")), right = numbers(*c.find("right"));
        double worst = 0;
        for (int k = 0; k < head.points(); ++k) {
            worst = std::max(worst,
                             std::abs(head.left(d)[static_cast<std::size_t>(k)] - left[static_cast<std::size_t>(k)]));
            worst = std::max(worst,
                             std::abs(head.right(d)[static_cast<std::size_t>(k)] - right[static_cast<std::size_t>(k)]));
        }
        CHECK_MESSAGE(worst < 2e-3, "case " << d << ": " << worst << " dB off");  // the fixtures carry four decimals
        CHECK_MESSAGE(std::abs(head.itd(d) - c.find("itd")->numberOr(0)) < 2e-3,
                      "case " << d << ": itd " << head.itd(d));
        ++d;
    }
}

TEST_CASE("another axis holds the model's values at shared points and the end values beyond the ends") {
    //  Catches: a resampler that reads the wrong channel's row, or is off by one point.
    HeadModel m = shipped();
    m.notches.list.clear();  // a peak-shaped notch's tails reach everywhere; the tables alone hold beyond the ends
    const std::vector<Vec3> dirs{direction(40, 15), direction(-110, -30)};
    Head full;
    full.prepare(m, dirs);
    std::vector<double> erbs{m.axis.erb0 - 1.0};
    for (int k = 0; k < m.axis.points; k += 7) erbs.push_back(m.axis.erb0 + k * m.axis.step);
    erbs.push_back(m.axis.erb0 + (m.axis.points - 1) * m.axis.step + 2.0);
    Head sparse;
    sparse.prepare(m, dirs, erbs);
    REQUIRE(sparse.points() == static_cast<int>(erbs.size()));
    full.synthesise(0.4, 1.1);
    sparse.synthesise(0.4, 1.1);
    for (int d = 0; d < 2; ++d) {
        for (std::size_t i = 1; i + 1 < erbs.size(); ++i) {
            const std::size_t k = (i - 1) * 7;
            CHECK(sparse.left(d)[i] == doctest::Approx(full.left(d)[k]).epsilon(1e-9));
            CHECK(sparse.right(d)[i] == doctest::Approx(full.right(d)[k]).epsilon(1e-9));
        }
        CHECK(sparse.left(d).front() == doctest::Approx(full.left(d).front()).epsilon(1e-9));
        CHECK(sparse.right(d).back() == doctest::Approx(full.right(d).back()).epsilon(1e-9));
    }
}

TEST_CASE("a mirrored direction swaps the ears and negates the time difference") {
    //  Catches: an azimuth or y sign slipped between the page's convention and the core's.
    const std::vector<Vec3> dirs{direction(35, 10), direction(-35, 10), direction(140, -25), direction(-140, -25)};
    Head head;
    head.prepare(shipped(), dirs);
    head.synthesise(-0.3, 0.9);
    for (int d : {0, 2}) {
        CHECK(head.itd(d) == doctest::Approx(-head.itd(d + 1)).epsilon(1e-9));
        for (int k = 0; k < head.points(); k += 5) {
            CHECK(head.left(d)[static_cast<std::size_t>(k)] ==
                  doctest::Approx(head.right(d + 1)[static_cast<std::size_t>(k)]).epsilon(1e-9));
            CHECK(head.right(d)[static_cast<std::size_t>(k)] ==
                  doctest::Approx(head.left(d + 1)[static_cast<std::size_t>(k)]).epsilon(1e-9));
        }
    }
    CHECK(head.itd(0) < -100.0);  // the source on the left: the left ear hears it first
}

TEST_CASE("a file that does not fit together is refused with a reason") {
    //  Catches: a loader that trusts the sizes and later reads past a table.
    HeadModel m = shipped();
    std::string why;
    REQUIRE(m.consistent(&why));
    m.smooth.coef.pop_back();
    CHECK_FALSE(m.consistent(&why));
    CHECK(why.find("smooth") != std::string::npos);

    HeadModel out;
    std::string error;
    CHECK_FALSE(parseHeadModel(R"({"version": 2})", out, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parseHeadModel("not json", out, &error));

    std::string text = readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json");
    const std::size_t at = text.find("\"peak\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, 6, "\"bell\"");
    CHECK_FALSE(parseHeadModel(text, out, &error));
    CHECK(error.find("peak") != std::string::npos);
}
