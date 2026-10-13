// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "bambi/dsp/resample.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("a sine resampled from 44.1 to 48 kHz is the sine at 48 kHz, and back down it folds nothing") {
    //  Catches: the output's time axis scaled the wrong way, a sinc without its window, a downsampling that keeps
    //  the input's band and aliases.
    const double pi = 3.14159265358979323846;
    std::vector<float> at441(4410);
    for (std::size_t i = 0; i < at441.size(); ++i)
        at441[i] = static_cast<float>(std::sin(2 * pi * 1000.0 * i / 44100.0));
    std::vector<float> at48;
    resampleSinc(at441, 44100.0, 48000.0, at48);
    REQUIRE(at48.size() == 4800);
    double err = 0, ref = 0;
    for (std::size_t i = 100; i + 100 < at48.size(); ++i) {  // the edges have no neighbours to sum over
        const double want = std::sin(2 * pi * 1000.0 * i / 48000.0);
        err += (at48[i] - want) * (at48[i] - want);
        ref += want * want;
    }
    CHECK(10 * std::log10(err / ref) < -60.0);

    //  a 23.5 kHz tone at 48 kHz is above the 44.1 kHz Nyquist: resampled down it must not come back folded to 20.6
    std::vector<float> high(4800);
    for (std::size_t i = 0; i < high.size(); ++i)
        high[i] = static_cast<float>(std::sin(2 * pi * 23500.0 * i / 48000.0));
    std::vector<float> down;
    resampleSinc(high, 48000.0, 44100.0, down);
    REQUIRE(down.size() == 4410);
    double energy = 0;
    for (std::size_t i = 100; i + 100 < down.size(); ++i) energy += down[i] * down[i];
    //  against a full-scale sine's power; measured -44 dB with the 32-sample half width
    CHECK(10 * std::log10(energy / (down.size() - 200) / 0.5) < -40.0);
}
