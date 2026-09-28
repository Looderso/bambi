// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "bambi/dsp/fft.hpp"
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

/*  A tone placed exactly on a bin is the check that catches a subtly wrong FFT instantly: the
 *  peak must be on that bin and nowhere else, at exactly N/2. */
TEST_CASE("a tone on an exact bin lands on that bin at the right magnitude") {
    for (std::size_t n : {64u, 256u, 2048u}) {
        RealFft f;
        f.prepare(n);
        REQUIRE(f.size() == n);
        REQUIRE(f.bins() == n / 2 + 1);

        for (std::size_t bin : {std::size_t{1}, n / 8, n / 4, n / 2 - 1}) {
            std::vector<double> x(n), mag(n / 2 + 1);
            for (std::size_t i = 0; i < n; ++i)
                x[i] = std::cos(2.0 * kPi * static_cast<double>(bin * i) / static_cast<double>(n));
            f.magnitude(x, mag);

            std::size_t peak = 0;
            for (std::size_t i = 0; i <= n / 2; ++i)
                if (mag[i] > mag[peak]) peak = i;

            INFO("n=", n, " bin=", bin);
            CHECK(peak == bin);
            CHECK(mag[peak] == doctest::Approx(static_cast<double>(n) / 2.0).epsilon(1e-4));

            for (std::size_t i = 0; i <= n / 2; ++i)  // and nothing anywhere else
                if (i != bin) CHECK(mag[i] < static_cast<double>(n) * 1e-3);
        }
    }
}

TEST_CASE("relative amplitudes are preserved") {
    RealFft f;
    f.prepare(2048);
    std::vector<double> x(2048), mag(1025);
    for (std::size_t i = 0; i < 2048; ++i)
        x[i] = std::cos(2.0 * kPi * 5.0 * static_cast<double>(i) / 2048.0) +
               0.3 * std::cos(2.0 * kPi * 69.0 * static_cast<double>(i) / 2048.0);
    f.magnitude(x, mag);
    CHECK(mag[69] / mag[5] == doctest::Approx(0.3).epsilon(1e-3));
}

TEST_CASE("dc and nyquist are not lost") {
    RealFft f;
    f.prepare(64);
    std::vector<double> x(64, 1.0), mag(33);
    f.magnitude(x, mag);
    CHECK(mag[0] == doctest::Approx(64.0).epsilon(1e-6));  // DC
    for (std::size_t i = 1; i <= 32; ++i) CHECK(mag[i] < 1e-6);

    for (std::size_t i = 0; i < 64; ++i) x[i] = (i % 2 == 0) ? 1.0 : -1.0;
    f.magnitude(x, mag);
    CHECK(mag[32] == doctest::Approx(64.0).epsilon(1e-6));  // Nyquist
}

TEST_CASE("silence in, silence out") {
    RealFft f;
    f.prepare(256);
    std::vector<double> x(256, 0.0), mag(129, 999.0);
    f.magnitude(x, mag);
    for (double m : mag) CHECK(m == doctest::Approx(0.0));
}

TEST_CASE("undersized buffers are refused rather than read past") {
    RealFft f;
    f.prepare(64);
    std::vector<double> tooShort(32, 1.0), mag(33, -1.0);
    f.magnitude(tooShort, mag);
    CHECK(mag[0] == doctest::Approx(-1.0));  // untouched

    std::vector<double> x(64, 1.0), tooFewBins(4, -1.0);
    f.magnitude(x, tooFewBins);
    CHECK(tooFewBins[0] == doctest::Approx(-1.0));
}

TEST_CASE("an unprepared instance is inert") {
    RealFft f;
    std::vector<double> x(64, 1.0), mag(33, -1.0);
    f.magnitude(x, mag);
    CHECK(f.size() == 0);
    CHECK(mag[0] == doctest::Approx(-1.0));
}

TEST_CASE("hann window is periodic and sums as expected") {
    std::vector<double> w(64);
    hannWindow(w);
    CHECK(w[0] == doctest::Approx(0.0));
    CHECK(w[32] == doctest::Approx(1.0));
    CHECK(w[16] == doctest::Approx(0.5));
    double sum = 0;
    for (double v : w) sum += v;
    CHECK(sum == doctest::Approx(32.0).epsilon(1e-9));  // periodic Hann sums to N/2
}
