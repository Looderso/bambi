// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <initializer_list>

#include "bambi/dsp/biquad.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
constexpr double kFs = 48000.0;
double dB(double magnitude) { return 20.0 * std::log10(magnitude); }

//  What the section actually does to a sine, measured -- so magnitudeAt, which the other tests lean
//  on, is itself checked against the filter it describes.
double measured(const BiquadCoeffs& c, double hz) {
    BiquadState s;
    double peak = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const double y = run(c, s, std::sin(2.0 * 3.14159265358979323846 * hz * i / kFs));
        if (i > 40000) peak = std::max(peak, std::abs(y));
    }
    return peak;
}
}  // namespace

TEST_CASE("a shelf has its gain on its own side and none on the other, and never overshoots") {
    for (const double gain : {-9.0, -2.5, 4.0}) {
        const BiquadCoeffs low = lowShelf(250.0, gain, kFs), high = highShelf(4000.0, gain, kFs);
        CHECK(dB(magnitudeAt(low, 5.0, kFs)) == doctest::Approx(gain).epsilon(0.01));
        CHECK(std::abs(dB(magnitudeAt(low, 20000.0, kFs))) < 0.01);
        CHECK(dB(magnitudeAt(high, 22000.0, kFs)) == doctest::Approx(gain).epsilon(0.01));
        CHECK(std::abs(dB(magnitudeAt(high, 10.0, kFs))) < 0.01);
        //  half the gain at the corner, and between 0 and the gain everywhere: a slope of 1 has no bump
        CHECK(dB(magnitudeAt(low, 250.0, kFs)) == doctest::Approx(gain / 2.0).epsilon(0.02));
        CHECK(dB(magnitudeAt(high, 4000.0, kFs)) == doctest::Approx(gain / 2.0).epsilon(0.02));
        for (double hz = 10.0; hz < 23000.0; hz *= 1.1)
            for (const BiquadCoeffs& c : {low, high}) {
                const double g = dB(magnitudeAt(c, hz, kFs));
                CHECK(g <= std::max(0.0, gain) + 1e-6);
                CHECK(g >= std::min(0.0, gain) - 1e-6);
            }
    }
}

TEST_CASE("a low-pass and a high-pass are 3 dB down at their corner and fall at 12 dB an octave") {
    const double q = std::sqrt(0.5);
    const BiquadCoeffs lp = lowPass(1000.0, q, kFs), hp = highPass(1000.0, q, kFs);
    CHECK(dB(magnitudeAt(lp, 1000.0, kFs)) == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(dB(magnitudeAt(hp, 1000.0, kFs)) == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(std::abs(dB(magnitudeAt(lp, 20.0, kFs))) < 0.01);
    CHECK(std::abs(dB(magnitudeAt(hp, 20000.0, kFs))) < 0.01);
    CHECK(dB(magnitudeAt(lp, 8000.0, kFs)) - dB(magnitudeAt(lp, 4000.0, kFs)) == doctest::Approx(-12.0).epsilon(0.1));
    CHECK(dB(magnitudeAt(hp, 62.5, kFs)) - dB(magnitudeAt(hp, 125.0, kFs)) == doctest::Approx(-12.0).epsilon(0.02));
}

TEST_CASE("what magnitudeAt says is what the section does to a sine") {
    const BiquadCoeffs c = highShelf(4000.0, -6.0, kFs);
    for (const double hz : {300.0, 4001.0, 11003.0})
        CHECK(measured(c, hz) == doctest::Approx(magnitudeAt(c, hz, kFs)).epsilon(0.002));
    const BiquadCoeffs l = lowShelf(250.0, 3.0, kFs);
    CHECK(measured(l, 60.0) == doctest::Approx(magnitudeAt(l, 60.0, kFs)).epsilon(0.002));
}
