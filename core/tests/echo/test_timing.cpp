// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/timing.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("a synced tap lasts its steps, and swing rides the offset") {
    TapTiming t;
    t.steps = 3;  // a dotted eighth
    t.offsetSteps = 2;
    t.swing = 0.5;
    const TapTimes at120 = tapTimes(t, 120.0, 3);
    CHECK(at120.periodSeconds == doctest::Approx(0.375));
    CHECK(at120.offsetSeconds == doctest::Approx(2.5 * 0.125));
    CHECK(at120.stepsPlayed == 3);
    //  the loop stays even whatever the swing: it moves where the tap sits, not how long it lasts
    t.swing = 0.0;
    CHECK(tapTimes(t, 120.0, 3).periodSeconds == at120.periodSeconds);
    //  50 % is straight against a tap on the downbeat, 67 % the shuffle
    TapTiming pong;
    pong.steps = 4, pong.offsetSteps = 2;
    CHECK(placeInLoop(tapTimes(pong, 120.0, 3)) == doctest::Approx(0.5));
    pong.swing = 0.68;
    CHECK(placeInLoop(tapTimes(pong, 120.0, 3)) == doctest::Approx(0.67));
}

TEST_CASE("the longest tap falls as the order rises, and a synced one too long is halved onto the grid") {
    CHECK(maxTapSeconds(1) == 2.4);
    CHECK(maxTapSeconds(3) == 2.4);
    CHECK(maxTapSeconds(4) == 1.6);
    CHECK(maxTapSeconds(5) == 1.6);
    CHECK(maxTapSeconds(6) == 1.2);
    CHECK(maxTapSeconds(7) == 1.2);

    TapTiming bar;
    bar.steps = 16;
    //  a bar at 120 is 2 s: it fits order 3, is halved once at order 5 (1.6 s) and twice at 7 (1.2 s)
    CHECK(tapTimes(bar, 120.0, 3).stepsPlayed == 16);
    CHECK(tapTimes(bar, 120.0, 5).stepsPlayed == 8);
    CHECK(tapTimes(bar, 120.0, 5).periodSeconds == doctest::Approx(1.0));
    CHECK(tapTimes(bar, 120.0, 7).stepsPlayed == 8);
    CHECK(tapTimes(bar, 90.0, 7).stepsPlayed == 4);  // 2.67 s -> 1.33 -> 0.67
    //  below 100 bpm a bar is over 2.4 s even at order 3, so it is halved too
    CHECK(tapTimes(bar, 80.0, 3).stepsPlayed == 8);
    CHECK(tapTimes(bar, 80.0, 3).periodSeconds == doctest::Approx(1.5));
    //  what is stored is never touched: the same tap at another tempo or order is a bar again
    CHECK(bar.steps == 16);

    //  the other end: above 750 bpm a sixteenth is under 20 ms, and is doubled the same way
    TapTiming one;
    one.steps = 1;
    CHECK(tapTimes(one, 900.0, 3).stepsPlayed == 2);
    CHECK(tapTimes(one, 900.0, 3).periodSeconds >= kMinTapSeconds);

    //  a free time is clamped, to the order's cap
    TapTiming free;
    free.synced = false;
    free.ms = 2400.0;
    free.offsetMs = 2000.0;
    CHECK(tapTimes(free, 120.0, 3).periodSeconds == 2.4);
    CHECK(tapTimes(free, 120.0, 7).periodSeconds == 1.2);
    CHECK(tapTimes(free, 120.0, 7).offsetSeconds == 1.2);
    CHECK(tapTimes(free, 120.0, 7).stepsPlayed == 0);
    free.ms = 1.0;
    CHECK(tapTimes(free, 120.0, 3).periodSeconds == kMinTapSeconds);
}

TEST_CASE("leaving sync moves nothing, and returning snaps to the nearest step") {
    TapTiming t;
    t.steps = 6, t.offsetSteps = 3, t.swing = 0.25;
    const TapTimes before = tapTimes(t, 96.0, 3);
    const TapTiming left = leaveSync(t, 96.0);
    CHECK_FALSE(left.synced);
    const TapTimes after = tapTimes(left, 96.0, 3);
    CHECK(after.periodSeconds == doctest::Approx(before.periodSeconds));
    CHECK(after.offsetSeconds == doctest::Approx(before.offsetSeconds));
    //  the synced values are kept, so the greyed side still says what a return would land on
    CHECK(left.steps == 6);

    //  and back: exactly where it was
    const TapTiming back = returnToSync(left, 96.0);
    CHECK(back.synced);
    CHECK(back.steps == 6);
    CHECK(back.offsetSteps == 3);
    CHECK(back.swing == doctest::Approx(0.25));

    //  nearest, not floor: 2.6 steps is 3
    TapTiming drift = left;
    drift.ms = 2.6 * stepSeconds(96.0) * 1000.0;
    CHECK(returnToSync(drift, 96.0).steps == 3);
    drift.ms = 2.4 * stepSeconds(96.0) * 1000.0;
    CHECK(returnToSync(drift, 96.0).steps == 2);
    //  the offset's whole steps and its remainder, the remainder held under a step
    drift.offsetMs = 1.97 * stepSeconds(96.0) * 1000.0;
    CHECK(returnToSync(drift, 96.0).offsetSteps == 1);
    CHECK(returnToSync(drift, 96.0).swing == doctest::Approx(kMaxSwing));
    //  and a time no step count reaches is held to the grid's ends
    drift.ms = 9000.0;
    CHECK(returnToSync(drift, 96.0).steps == kMaxSteps);
    drift.ms = 1.0;
    CHECK(returnToSync(drift, 96.0).steps == 1);
}

TEST_CASE("steps are named as a musician would, and by the fraction otherwise") {
    CHECK(stepName(1) == "1/16");
    CHECK(stepName(2) == "1/8");
    CHECK(stepName(3) == "1/8.");
    CHECK(stepName(4) == "1/4");
    CHECK(stepName(6) == "1/4.");
    CHECK(stepName(8) == "1/2");
    CHECK(stepName(12) == "1/2.");
    CHECK(stepName(16) == "1 bar");
    CHECK(stepName(5) == "5/16");
    CHECK(stepName(15) == "15/16");
    CHECK(stepName(0).empty());
    CHECK(stepName(17).empty());
}
