// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/timeline.hpp"
#include "doctest.h"

using namespace bambi::live;

TEST_CASE("a song position reads as Live shows it, each part from 1") {
    //  Catches: a part counted from 0, a sixteenth read as an eighth, a bar of three.
    CHECK(barsBeats(0.0) == "1.1.1");
    CHECK(barsBeats(4.0) == "2.1.1");
    CHECK(barsBeats(5.75) == "2.2.4");
    CHECK(barsBeats(-1.0) == "1.1.1");
}

TEST_CASE("the song is as long as its last clip, locator or loop, in whole bars, never too short to draw") {
    //  Catches: the loop's end or a locator past the clips ignored, a part bar not rounded up.
    Arrangement a;
    CHECK(songLength(a) == 64.0);
    a.tracks = {{1, {{0.0, 70.0, "a", "#000000", false}}}};
    CHECK(songLength(a) == 72.0);
    a.locators = {{100.0, "end"}};
    CHECK(songLength(a) == 100.0);
    a.loopStart = 96.0;
    a.loopLength = 16.0;
    CHECK(songLength(a) == 112.0);
}

TEST_CASE("a ruler click locates to a near locator, else to the grid, never before the start") {
    //  Catches: the locator not winning within its reach, one won from beyond it, a negative position.
    Arrangement a;
    a.locators = {{30.0, "verse"}};
    CHECK(locateAt(29.0, a, 4.0, 1.5) == 30.0);
    CHECK(locateAt(27.0, a, 4.0, 1.5) == 28.0);
    CHECK(locateAt(-3.0, a, 4.0, 1.5) == 0.0);
    CHECK(locateAt(5.3, a, 0.0, 0.0) == 5.3);
}

TEST_CASE("the playhead heard trails the one played by the samples to the speakers") {
    //  Catches: the lateness added, or taken in seconds rather than at the tempo.
    CHECK(heardAt(10.0, 120.0, 48000.0, 24000) == 9.0);
    CHECK(heardAt(10.0, 120.0, 0.0, 24000) == 10.0);
}

TEST_CASE("a point snaps to its own track's clip edges and the locators within reach, else to the grid") {
    //  Catches: another track's clip edge taken, an edge won from beyond its reach, the grid ignored.
    Arrangement a;
    a.tracks = {{1, {{10.0, 22.0, "a", "#000000", false}}}, {2, {{13.0, 14.0, "b", "#000000", false}}}};
    a.locators = {{30.0, "verse"}};
    CHECK(snapAt(12.6, edgesOf(a, 2), 4.0, 0.5) == 13.0);
    CHECK(snapAt(12.6, edgesOf(a, 1), 4.0, 0.5) == 12.0);
    CHECK(snapAt(21.7, edgesOf(a, 1), 4.0, 0.5) == 22.0);
    CHECK(snapAt(29.6, edgesOf(a, 1), 1.0, 0.5) == 30.0);
}

TEST_CASE("the grid follows how wide a beat is drawn") {
    //  Catches: a bar grid on a wide drawing, a sixteenth on a narrow one.
    CHECK(gridFor(5.0) == 4.0);
    CHECK(gridFor(20.0) == 1.0);
    CHECK(gridFor(60.0) == 0.25);
}
