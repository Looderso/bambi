// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "bambi/decode/sofafile.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

/*  The two fixtures beside this test were written by make-sofa.py: eight directions, two receivers, 64 samples
 *  at 48 kHz, datasets compressed, and sample n of measurement m at the ear e (0 left, 1 right) equal to
 *  (m + 1)(e + 1) / 1000 + n / 100000. made-swapped.sofa puts the right ear first in the file, which the convention forbids.
 */
namespace {
const std::vector<std::pair<double, double>> kDirections{{0, 0},  {90, 0},  {-90, 0}, {180, 0},
                                                         {0, 90}, {0, -90}, {45, 30}, {-135, -20}};

void checkMade(const MeasuredHead& m) {
    REQUIRE(m.directions.size() == 8);
    CHECK(m.taps == 64);
    CHECK(m.sampleRate == doctest::Approx(48000.0));
    for (std::size_t d = 0; d < 8; ++d) {
        const Vec3 want = direction(kDirections[d].first, kDirections[d].second);
        CHECK(std::abs(m.directions[d].x - want.x) < 1e-5);
        CHECK(std::abs(m.directions[d].y - want.y) < 1e-5);
        CHECK(std::abs(m.directions[d].z - want.z) < 1e-5);
        for (std::size_t n : {std::size_t{0}, std::size_t{17}, std::size_t{63}}) {
            const double md = static_cast<double>(d), nd = static_cast<double>(n);
            CHECK(m.left[d * 64 + n] == doctest::Approx((md + 1) * 1 / 1000.0 + nd / 100000.0).epsilon(1e-6));
            CHECK(m.right[d * 64 + n] == doctest::Approx((md + 1) * 2 / 1000.0 + nd / 100000.0).epsilon(1e-6));
        }
    }
}
}  // namespace

TEST_CASE("a SOFA file is read as a measured head: directions, both ears in order, the rate, the samples") {
    //  Catches: spherical positions taken as cartesian or azimuth the wrong way round, receivers and samples
    //  interleaved the wrong way, the ears swapped, compressed datasets not inflated, a wrong-way file read as
    //  if it were right.
    MeasuredHead m;
    std::string error;
    REQUIRE_MESSAGE(readSofa(std::string(BAMBI_CORE_DIR) + "/tests/decode/made.sofa", m, &error), error);
    CHECK(m.name == "made.sofa");
    checkMade(m);

    //  the right ear first breaks the convention the reader stands on, and is refused rather than read wrong
    MeasuredHead swapped;
    CHECK_FALSE(readSofa(std::string(BAMBI_CORE_DIR) + "/tests/decode/made-swapped.sofa", swapped, &error));
    CHECK(error.find("check") != std::string::npos);

    //  the same bytes through readSofaData
    const std::string bytes = readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/made.sofa");
    MeasuredHead fromBytes;
    REQUIRE_MESSAGE(readSofaData(std::span<const char>(bytes.data(), bytes.size()), fromBytes, &error), error);
    checkMade(fromBytes);

    //  not a SOFA file: refused with a reason, not a crash
    MeasuredHead none;
    CHECK_FALSE(readSofa(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json", none, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(readSofa("/nowhere/at/all.sofa", none, &error));
}
