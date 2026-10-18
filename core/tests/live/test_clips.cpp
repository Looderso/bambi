// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/clips.hpp"
#include "doctest.h"

using namespace bambi::live;

TEST_CASE("a track's clip runs on from its last report at the tempo, only while that slot plays") {
    //  Catches: a report replacing another track's, the position taken from the wrong slot, a stopped track still
    //  playing, the samples run at the wrong rate.
    ClipClock clock;
    clock.receive({1, 2, 1.0, 0.0, 4.0, true}, 48000);
    clock.receive({7, 0, 0.0, 0.0, 8.0, true}, 0);
    CHECK(clock.playing(1) == 2);
    CHECK(clock.playing(9) == -1);
    //  120 bpm at 48 kHz: a quarter note every 24000 samples
    CHECK(*clock.position(1, 2, 48000 + 24000, 120.0, 48000.0) == 2.0);
    CHECK(*clock.position(1, 2, 48000 + 96000, 120.0, 48000.0) == 1.0);  // looped
    CHECK_FALSE(clock.position(1, 0, 48000, 120.0, 48000.0).has_value());
    clock.receive({1, -1, 0.0, 0.0, 0.0, false}, 60000);
    CHECK(clock.playing(1) == -1);
    CHECK_FALSE(clock.position(1, 2, 60000, 120.0, 48000.0).has_value());
    CHECK(clock.playing(7) == 0);
}

TEST_CASE("a track stays the session's after its clip stops, until Back to Arrangement gives it back") {
    //  Catches: song lanes let go the moment a clip stops, or held after Back to Arrangement.
    ClipClock clock;
    CHECK_FALSE(clock.overridden(1));
    clock.receive({1, 0, 0.0, 0.0, 4.0, true, true}, 0);
    CHECK(clock.overridden(1));
    clock.receive({1, -1, 0.0, 0.0, 0.0, false, true}, 100);
    CHECK(clock.overridden(1));
    clock.receive({1, -1, 0.0, 0.0, 0.0, false, false}, 200);
    CHECK_FALSE(clock.overridden(1));
}
