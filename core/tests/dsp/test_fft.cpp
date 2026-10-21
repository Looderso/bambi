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
    //  Catches: the bins read one slot out of place.
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
    //  Catches: power returned where magnitude is asked for.
    RealFft f;
    f.prepare(2048);
    std::vector<double> x(2048), mag(1025);
    for (std::size_t i = 0; i < 2048; ++i)
        x[i] = std::cos(2.0 * kPi * 5.0 * static_cast<double>(i) / 2048.0) +
               0.3 * std::cos(2.0 * kPi * 69.0 * static_cast<double>(i) / 2048.0);
    f.magnitude(x, mag);
    CHECK(std::abs(mag[69] / mag[5] - 0.3) < 1e-3);
}

TEST_CASE("dc and nyquist are not lost") {
    //  Catches: the Nyquist bin read from the DC slot.
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
    //  Catches: anything added to the input on its way into the transform.
    RealFft f;
    f.prepare(256);
    std::vector<double> x(256, 0.0), mag(129, 999.0);
    f.magnitude(x, mag);
    for (double m : mag) CHECK(m == doctest::Approx(0.0));
}

TEST_CASE("undersized buffers are refused rather than read past") {
    //  Catches: the sizes not checked, so a short buffer is read and written past its end.
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
    //  Catches: an instance with no setup run anyway.
    RealFft f;
    std::vector<double> x(64, 1.0), mag(33, -1.0);
    f.magnitude(x, mag);
    CHECK(f.size() == 0);
    CHECK(mag[0] == doctest::Approx(-1.0));
}

TEST_CASE("a size the transform cannot do is refused, and leaves the instance inert") {
    //  Catches: a size that is not a multiple of 32 set up anyway, which pffft only asserts against and a release
    //  build lets through -- 100 would then transform wrongly.
    RealFft f;
    for (const std::size_t size : {std::size_t{0}, std::size_t{48}, std::size_t{100}, std::size_t{7 * 32}}) {
        CHECK_FALSE(f.prepare(size));
        CHECK(f.size() == 0);
    }
    std::vector<double> x(100, 1.0), mag(51, -1.0);
    f.magnitude(x, mag);
    CHECK(mag[0] == -1.0);  // untouched
    CHECK(f.prepare(96));   // 32 times 3: a size it can do
    CHECK(f.size() == 96);
}

TEST_CASE("hann window is periodic and sums as expected") {
    //  Catches: the symmetric Hann window in place of the periodic one.
    std::vector<double> w(64);
    hannWindow(w);
    CHECK(w[0] == doctest::Approx(0.0));
    CHECK(w[32] == doctest::Approx(1.0));
    CHECK(w[16] == doctest::Approx(0.5));
    double sum = 0;
    for (double v : w) sum += v;
    CHECK(sum == doctest::Approx(32.0).epsilon(1e-9));  // periodic Hann sums to N/2
}

TEST_CASE("the inverse undoes the forward transform, and a linear phase is a delay") {
    //  Catches: the Nyquist bin read from the wrong slot, a missing 1/N, a conjugated spectrum.
    RealFft fft;
    fft.prepare(64);
    std::vector<double> x(64), re(33), im(33), back(64);
    for (std::size_t i = 0; i < 64; ++i) x[i] = std::sin(0.37 * static_cast<double>(i)) + (i == 5 ? 1.0 : 0.0);
    fft.forward(x, re, im);
    fft.inverse(re, im, back);
    for (std::size_t i = 0; i < 64; ++i) CHECK(std::abs(back[i] - x[i]) < 1e-5);

    for (std::size_t k = 0; k < 33; ++k) {  // e^{-j 2 pi k 7 / 64}: an impulse seven samples late
        re[k] = std::cos(2.0 * kPi * static_cast<double>(k) * 7.0 / 64.0);
        im[k] = -std::sin(2.0 * kPi * static_cast<double>(k) * 7.0 / 64.0);
    }
    fft.inverse(re, im, back);
    for (std::size_t i = 0; i < 64; ++i) CHECK(std::abs(back[i] - (i == 7 ? 1.0 : 0.0)) < 1e-5);
}

TEST_CASE("the FFT's own layout: a round trip scales by N, and convolve-accumulate is a circular convolution") {
    //  Catches: the packed DC and Nyquist pair multiplied as one complex number, a scale applied twice.
    RealFft fft;
    fft.prepare(64);
    AlignedFloats a, b, sa, sb, ab, y;
    for (AlignedFloats* f : {&a, &b, &sa, &sb, &ab, &y}) f->prepare(64);
    for (std::size_t i = 0; i < 64; ++i) {
        a.data()[i] = static_cast<float>(std::sin(0.3 * static_cast<double>(i)) + (i == 3 ? 0.5 : 0.0));
        b.data()[i] = i < 5 ? static_cast<float>(1.0 / (1.0 + static_cast<double>(i))) : 0.0f;
    }
    fft.spectrum(a.data(), sa.data());
    fft.signal(sa.data(), y.data());
    for (std::size_t i = 0; i < 64; ++i) CHECK(std::abs(y.data()[i] - 64.0f * a.data()[i]) < 64.0f * 1e-4f);

    fft.spectrum(b.data(), sb.data());
    fft.convolveAccumulate(sa.data(), sb.data(), ab.data(), 1.0f / 64.0f);
    fft.signal(ab.data(), y.data());
    for (std::size_t n = 0; n < 64; ++n) {
        double direct = 0;
        for (std::size_t k = 0; k < 5; ++k) direct += b.data()[k] * a.data()[(n + 64 - k) % 64];
        CHECK(std::abs(y.data()[n] - direct) < 1e-4);
    }
}
