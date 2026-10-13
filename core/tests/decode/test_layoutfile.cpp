// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/decode/layoutfile.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

namespace {
const char* kAmbdec = R"(# AmbDec configuration, as the Ambisonic Decoder Toolbox writes one
/description      a square and a top
/version          3
/dec/chan_mask    fff
/dec/freq_bands   2
/dec/speakers     5
/dec/coeff_scale  fuma
/opt/input_scale  fuma
/opt/nfeff_comp   output
/opt/delay_comp   on
/opt/level_comp   on
/opt/xover_freq   400.0
/opt/xover_ratio  0.0
/speakers/{
add_spkr    FL    2.000    45.0    0.0    system:playback_1

add_spkr    FR    2.000   -45.0    0.0    system:playback_2   # the right one
add_spkr    BL    2.500   135.0    0.0    system:playback_7
add_spkr    BR    2.500  -135.0    0.0
add_spkr    TOP   1.800     0.0   90.0    system:playback_9
/}
/lfmatrix/{
order_gain     1.00000  1.00000  0.00000  0.00000
add_row  0.1 0.2 0.3 0.4
/}
/hfmatrix/{
order_gain     1.00000  1.00000  0.00000  0.00000
add_row  0.1 0.2 0.3 0.4
/}
/end
)";
}  // namespace

TEST_CASE("an AmbDec configuration is read as a layout: positions, distances, and the channel from the port") {
    //  Catches: the azimuth negated (AmbDec's is counter-clockwise, as ours), distance and azimuth swapped, the
    //  channel taken from the position when the port names one, the matrices read as speakers, a comment or a
    //  blank line that trips the reader.
    Layout l;
    std::string error;
    REQUIRE_MESSAGE(parseAmbdecLayout(kAmbdec, l, &error), error);
    REQUIRE(l.speakers.size() == 5);
    CHECK(l.speakers[0].azimuthDeg == doctest::Approx(45.0));
    CHECK(l.speakers[0].radiusM == doctest::Approx(2.0));
    CHECK(l.speakers[0].channel == 1);
    CHECK(l.speakers[1].azimuthDeg == doctest::Approx(-45.0));
    CHECK(l.speakers[1].channel == 2);
    CHECK(l.speakers[2].azimuthDeg == doctest::Approx(135.0));
    CHECK(l.speakers[2].radiusM == doctest::Approx(2.5));
    CHECK(l.speakers[2].channel == 7);  // the port says so
    CHECK(l.speakers[3].channel == 4);  // no port: its position
    CHECK(l.speakers[4].elevationDeg == doctest::Approx(90.0));
    CHECK(l.speakers[4].channel == 9);
    for (const Loudspeaker& s : l.speakers) {
        CHECK_FALSE(s.imaginary);
        CHECK(s.gain == doctest::Approx(1.0));
    }

    //  parseLayoutFile tells the two formats apart by their first character
    Layout any;
    REQUIRE(parseLayoutFile(kAmbdec, any, &error));
    CHECK(any.speakers.size() == 5);
    REQUIRE(parseLayoutFile(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16.json"), any, &error));
    CHECK(any.speakers.size() == 17);
}

TEST_CASE("an AmbDec configuration that does not hold together is refused with the reason") {
    //  Catches: a count that disagrees with the block let through, a non-number read as zero, JSON or prose read as
    //  a layout.
    Layout l;
    std::string error;
    std::string wrongCount(kAmbdec);
    wrongCount.replace(wrongCount.find("/dec/speakers     5"), 19, "/dec/speakers     4");
    CHECK_FALSE(parseAmbdecLayout(wrongCount, l, &error));
    CHECK(error.find("4") != std::string::npos);
    std::string notANumber(kAmbdec);
    notANumber.replace(notANumber.find("2.000    45.0"), 13, "2.000    left");
    CHECK_FALSE(parseAmbdecLayout(notANumber, l, &error));
    CHECK(error.find("FL") != std::string::npos);
    CHECK_FALSE(parseAmbdecLayout("just some text\nwith no block\n", l, &error));
    CHECK_FALSE(parseLayoutFile("{\"not\": \"a layout\"}", l, &error));
    CHECK_FALSE(parseAmbdecLayout("/speakers/{\n/}\n", l, &error));
}
