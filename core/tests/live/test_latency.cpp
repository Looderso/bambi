// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The host's delay compensation: every path from a source to the output takes as long as every other.

#include <algorithm>
#include <random>
#include <vector>

#include "bambi/live/latency.hpp"
#include "doctest.h"

using namespace bambi::live;

namespace {

/// Every path's length: each source dry, and each source through each send.
std::vector<int> paths(const HostLatency& l, const Compensation& c) {
    std::vector<int> out;
    for (std::size_t s = 0; s < l.sources.size(); ++s) {
        const int atOutput = std::max(l.sources[s], 0) + c.sourceDelay[s];
        out.push_back(atOutput + c.dryDelay + std::max(l.main, 0));
        for (std::size_t b = 0; b < l.sends.size(); ++b)
            out.push_back(atOutput + std::max(l.sends[b], 0) + c.returnDelay[b] + std::max(l.main, 0));
    }
    return out;
}

}  // namespace

TEST_CASE("every path from a source to the output takes the same time, and that time is the total") {
    //  Catches: a delay with the wrong sign or a missing term anywhere -- a source's, the dry sum's, a return's
    //  -- which leaves a send smeared against the dry signal or one source late against the others.
    std::mt19937 random(7);
    std::uniform_int_distribution<int> length(0, 4096), count(0, 6);
    for (int round = 0; round < 200; ++round) {
        HostLatency l;
        for (int i = count(random) + 1; i > 0; --i) l.sources.push_back(length(random));
        for (int i = count(random); i > 0; --i) l.sends.push_back(length(random));
        l.main = length(random);
        const Compensation c = compensate(l);
        for (const int p : paths(l, c)) CHECK(p == c.total);
    }
}

TEST_CASE("the slowest path at each meeting place is not delayed at all") {
    //  Catches: lining up to anything longer than the slowest path, which plays the whole mix later than
    //  it has to.
    HostLatency l{{0, 64, 10}, {128, 0, 300}, 64};
    const Compensation c = compensate(l);
    CHECK(c.sourceDelay == std::vector<int>{64, 0, 54});
    CHECK(c.dryDelay == 300);
    CHECK(c.returnDelay == std::vector<int>{172, 300, 0});
    CHECK(c.total == 64 + 300 + 64);
}

TEST_CASE("without sends nothing waits for them, and a negative report counts as none") {
    //  Catches: a dry delay left at a default, or a negative latency that shortens a path.
    HostLatency l{{-5, 32}, {}, -1};
    const Compensation c = compensate(l);
    CHECK(c.sourceDelay == std::vector<int>{32, 0});
    CHECK(c.dryDelay == 0);
    CHECK(c.returnDelay.empty());
    CHECK(c.total == 32);
}
