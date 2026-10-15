// SPDX-License-Identifier: GPL-3.0-or-later
//
//  A message's arrival, in the host's samples.

#include "bambi/live/arrival.hpp"
#include "doctest.h"

using bambi::live::Arrivals;

TEST_CASE("a line that came 10 ms before the block arrived 480 samples before it, at 48 kHz") {
    //  Catches: the time ago added instead of taken, or counted at a rate other than the host's.
    Arrivals a(48000.0);
    CHECK(a.at(10000, 0.010) == 9520);
}

TEST_CASE("the offset moves every arrival later by Live's way to the host") {
    //  Catches: the offset ignored, or taken away.
    Arrivals a(48000.0, 300);
    CHECK(a.at(10000, 0.0) == 10300);
}

TEST_CASE("an arrival is never earlier than the last, and a stamp from after the block counts as at it") {
    //  Catches: a message stamped earlier but taken later handed to the projector out of order, or a stamp from
    //  the future placing a message after the block it is played in.
    Arrivals a(48000.0);
    CHECK(a.at(10000, 0.001) == 9952);
    CHECK(a.at(10000, 0.002) == 9952);
    CHECK(a.at(10064, -0.5) == 10064);
}
