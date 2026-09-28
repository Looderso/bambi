// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <set>

#include "bambi/reverb/tailplan.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
bool prime(int n) {
    if (n < 2) return false;
    for (int i = 2; i * i <= n; ++i)
        if (n % i == 0) return false;
    return true;
}
constexpr double kFs = 48000.0;
}  // namespace

TEST_CASE("the lines are primes between 100 and 200 ms times the scale, no two alike, long and short interleaved") {
    for (const double scale : {0.55, 1.2, 2.0}) {
        const TailPlan p = planTail(16, kFs, scale, 0.06);
        std::set<int> seen;
        for (int k = 0; k < 16; ++k) {
            const int L = p.length[static_cast<std::size_t>(k)];
            CHECK(prime(L));
            CHECK(seen.insert(L).second);
            CHECK(L >= static_cast<int>(0.100 * scale * kFs) - 1);
            CHECK(L <= static_cast<int>(0.200 * scale * kFs * 1.01));
        }
        //  not in order: a line's neighbour is never the next one up, which a Hadamard pair would make alike
        int rising = 0;
        for (int k = 1; k < 16; ++k)
            rising += p.length[static_cast<std::size_t>(k)] > p.length[static_cast<std::size_t>(k - 1)] ? 1 : 0;
        CHECK(rising < 12);
        CHECK(rising > 3);
    }
}

TEST_CASE("sixteen lines at the hall's scale are enough for the longest decay the presets ask") {
    //  Schroeder and Logan: a total delay of at least 0.15 x RT60 for a tail without colour. 16 lines of
    //  100 to 200 ms are 2.3 s at a scale of 1, which carries decays to 15 s.
    const TailPlan one = planTail(16, kFs, 1.0, 0.06);
    CHECK(one.totalSeconds(kFs) == doctest::Approx(2.3).epsilon(0.03));
    CHECK(one.totalSeconds(kFs) / 0.15 > 15.0);
    const TailPlan cathedral = planTail(16, kFs, 2.0, 0.08);
    CHECK(cathedral.totalSeconds(kFs) / 0.15 > 6.5);
}

TEST_CASE("the diffuser's four steps add up to the mixing time, each spread over its range") {
    for (const double mixing : {0.025, 0.08}) {
        const TailPlan p = planTail(16, kFs, 1.0, mixing);
        double longest = 0.0;
        for (int s = 0; s < kDiffuserSteps; ++s) {
            const auto& d = p.diffuserDelay[static_cast<std::size_t>(s)];
            const int most = *std::max_element(d.begin(), d.begin() + 16),
                      least = *std::min_element(d.begin(), d.begin() + 16);
            const double range = (20.0 * (1 << s)) * mixing / 0.3 / 1000.0 * kFs;
            CHECK(most <= static_cast<int>(range) + 1);
            CHECK(most > 0.85 * range);       // the last sixteenth of the range is used
            CHECK(least < 0.15 * range + 2);  // and the first
            longest += most;
        }
        CHECK(longest / kFs == doctest::Approx(mixing).epsilon(0.08));
    }
}

TEST_CASE("the shuffles are shuffles, the signs are signs, and the same room is always the same network") {
    const TailPlan p = planTail(16, kFs, 1.2, 0.06);
    const auto permutation = [](const std::array<int, kMaxTailLines>& a) {
        std::set<int> s(a.begin(), a.begin() + 16);
        return s.size() == 16 && *s.begin() == 0 && *s.rbegin() == 15;
    };
    for (int s = 0; s < kDiffuserSteps; ++s) {
        CHECK(permutation(p.diffuserFrom[static_cast<std::size_t>(s)]));
        int plus = 0;
        for (int k = 0; k < 16; ++k) {
            const float g = p.diffuserSign[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)];
            CHECK((g == 1.0f || g == -1.0f));
            plus += g > 0 ? 1 : 0;
        }
        //  both signs are there. No more is asked: the seed gives its third step two pluses in
        //  sixteen, and that network is the one pinned below.
        CHECK(plus >= 1);
        CHECK(plus <= 15);
    }
    CHECK(permutation(p.outFrom));
    std::set<int> allpasses(p.allpass.begin(), p.allpass.begin() + 16);
    CHECK(allpasses.size() == 16);
    for (const int a : allpasses) CHECK(prime(a));

    //  the same twice; and a number pinned, so that a change to the generator or to the order the
    //  numbers are drawn in -- which would change every render ever made -- cannot pass unnoticed
    const TailPlan again = planTail(16, kFs, 1.2, 0.06);
    CHECK(again.length == p.length);
    CHECK(again.diffuserDelay == p.diffuserDelay);
    CHECK(again.outFrom == p.outFrom);
    CHECK(p.diffuserDelay[0][0] == 70);
    CHECK(p.allpass[0] == 521);
    CHECK(p.outFrom[0] == 12);
    CHECK(p.length[1] == 7963);
}

TEST_CASE("no two lines and no two allpasses are alike even where the primes are crowded") {
    //  32 of them in the smallest room at the lowest rate: the allpasses run from 22 to 66 samples, where
    //  rounding alone would hand out the same prime several times. Two lines alike are one resonance
    //  twice as loud.
    const TailPlan p = planTail(32, 44100.0, 0.1, 0.025);
    std::set<int> lines(p.length.begin(), p.length.begin() + 32), allpasses(p.allpass.begin(), p.allpass.begin() + 32);
    CHECK(lines.size() == 32);
    CHECK(allpasses.size() == 32);
}

TEST_CASE("the lines are a power of two, and nothing a plan asks for is longer than what is allocated") {
    CHECK(planTail(16, kFs, 1.0, 0.05).lines == 16);
    CHECK(planTail(20, kFs, 1.0, 0.05).lines == 16);
    CHECK(planTail(100, kFs, 1.0, 0.05).lines == 32);
    CHECK(planTail(1, kFs, 1.0, 0.05).lines == 2);
    for (const double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const TailLimits lim = tailLimits(fs);
        const TailPlan p = planTail(32, fs, 9.0, 1.0);  // past every limit: held to them
        for (int k = 0; k < 32; ++k) {
            CHECK(p.length[static_cast<std::size_t>(k)] < lim.line);
            CHECK(p.allpass[static_cast<std::size_t>(k)] < lim.allpass);
            for (int s = 0; s < kDiffuserSteps; ++s)
                CHECK(p.diffuserDelay[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] < lim.diffuser);
        }
    }
}
