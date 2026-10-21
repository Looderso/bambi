// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "bambi/decode/sofafile.hpp"
#include "bambi/math/sh.hpp"
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
            CHECK(std::abs(m.left[d * 64 + n] - ((md + 1) * 1 / 1000.0 + nd / 100000.0)) < 1e-8);
            CHECK(std::abs(m.right[d * 64 + n] - ((md + 1) * 2 / 1000.0 + nd / 100000.0)) < 1e-8);
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

TEST_CASE("a FIR-E file is read as a decoder matrix: inputs as emitters, outputs as receivers, taps in order") {
    //  Catches: the emitter dimension taken as the slowest instead of the fastest, inputs and outputs swapped, an
    //  attribute not read, a head file taken for a matrix.
    FirMatrix m;
    std::string error;
    REQUIRE_MESSAGE(readSofaMatrix(std::string(BAMBI_CORE_DIR) + "/tests/decode/made-matrix.sofa", m, &error), error);
    CHECK(m.inputs == 4);
    CHECK(m.outputs == 3);
    CHECK(m.taps == 32);
    CHECK(m.sampleRate == doctest::Approx(44100.0));
    CHECK(m.order == 1);
    CHECK(m.channelOrder == "ACN");
    CHECK(m.normalization == "N3D");
    for (int i = 0; i < 4; ++i)
        for (int o = 0; o < 3; ++o)
            for (int n : {0, 7, 31})
                CHECK(std::abs(m.filter(i, o)[static_cast<std::size_t>(n)] -
                               ((i + 1) / 10.0 + (o + 1) / 100.0 + n / 10000.0)) < 1e-7);
    FirMatrix notOne;
    CHECK_FALSE(readSofaMatrix(std::string(BAMBI_CORE_DIR) + "/tests/decode/made.sofa", notOne, &error));
    CHECK(error.find("FIR-E") != std::string::npos);
}

TEST_CASE("a matrix's filters are laid out for the field, each input scaled for N3D by its degree") {
    //  Catches: the factor on the wrong index (an output's instead of an input's), a file's extra inputs not
    //  dropped, a missing input's filter not silent, the field's channel count not the row count.
    FirMatrix m;
    m.sampleRate = 48000.0;
    m.inputs = 5;  // one more than a first-order field has
    m.outputs = 2;
    m.taps = 8;
    m.filters.resize(static_cast<std::size_t>(m.inputs * m.outputs * m.taps));
    for (std::size_t k = 0; k < m.filters.size(); ++k) m.filters[k] = 0.01f * static_cast<float>(k % 17) - 0.05f;
    std::vector<float> asN3d, plain;
    int taps = 0, tapsPlain = 0;
    CHECK(resampledFilters(m, 4, 48000.0, true, asN3d, taps) == 4);
    FirMatrix scaled = m;
    for (int i = 0; i < m.inputs; ++i)
        for (int o = 0; o < m.outputs; ++o)
            for (int t = 0; t < m.taps; ++t)
                scaled.filters[static_cast<std::size_t>((i * m.outputs + o) * m.taps + t)] *=
                    static_cast<float>(std::sqrt(2.0 * acnOrder(i) + 1.0));
    CHECK(resampledFilters(scaled, 4, 48000.0, false, plain, tapsPlain) == 4);
    REQUIRE(taps == tapsPlain);
    REQUIRE(asN3d.size() == static_cast<std::size_t>(4 * m.outputs * taps));
    CHECK(asN3d == plain);
    //  a wider field than the file: the extra channels' filters are silent
    std::vector<float> wide;
    CHECK(resampledFilters(m, 9, 48000.0, false, wide, taps) == 5);
    bool silent = true;
    for (std::size_t k = static_cast<std::size_t>(5 * m.outputs * taps); k < wide.size(); ++k)
        silent = silent && wide[k] == 0.0f;
    CHECK(silent);
}
