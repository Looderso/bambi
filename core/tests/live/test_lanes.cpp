// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>

#include "bambi/live/lanes.hpp"
#include "doctest.h"

using namespace bambi;
using namespace bambi::live;

TEST_CASE("a lane's value runs straight between points, holds before the first and after the last") {
    //  Catches: a step where a ramp belongs, a value before the first point taken as zero, the last not held.
    const std::vector<LanePoint> p{{4.0, -30.0}, {8.0, 30.0}, {12.0, 0.0}};
    CHECK_FALSE(valueAt({}, 1.0).has_value());
    CHECK(*valueAt(p, 0.0) == -30.0);
    CHECK(*valueAt(p, 6.0) == 0.0);
    CHECK(*valueAt(p, 10.0) == 15.0);
    CHECK(*valueAt(p, 99.0) == 0.0);
}

TEST_CASE("points stay in time order: put at their time, moved only between their neighbours, removed") {
    //  Catches: a point put out of order, a second at one time, a move past a neighbour, the wrong one removed.
    Lane l;
    CHECK(putPoint(l, {8.0, 1.0}) == 0);
    CHECK(putPoint(l, {2.0, 2.0}) == 0);
    CHECK(putPoint(l, {8.0, 3.0}) == 1);
    REQUIRE(l.points.size() == 2);
    CHECK(l.points[1].value == 3.0);
    putPoint(l, {12.0, 4.0});
    movePoint(l, 1, {20.0, 5.0});  // past its neighbour at 12
    CHECK(l.points[1].ppq < 12.0);
    CHECK(l.points[1].ppq > 8.0);
    CHECK(l.points[1].value == 5.0);
    movePoint(l, 0, {-3.0, 0.0});
    CHECK(l.points[0].ppq == 0.0);
    removePoint(l, 1);
    CHECK(l.points.size() == 2);
    CHECK(l.points[1].ppq == 12.0);
}

TEST_CASE("a press finds the point near it in time and value, the nearest in time when two are") {
    //  Catches: a point found from beyond its reach, or the farther of two.
    Lane l;
    putPoint(l, {4.0, 0.0});
    putPoint(l, {5.0, 0.0});
    CHECK(pointNear(l, 4.8, 0.1, 1.0, 0.2) == std::optional<std::size_t>{1});  // both in reach: the nearer
    CHECK_FALSE(pointNear(l, 4.5, 0.1, 0.2, 0.2).has_value());
    CHECK_FALSE(pointNear(l, 4.0, 0.9, 0.5, 0.2).has_value());
}

TEST_CASE("the lanes read back as written, and one the set cannot read is left out") {
    //  Catches: a field lost between the set and the host, the instance read as another, a broken lane taken.
    const std::vector<Lane> lanes{{1, Instance::Encoder, 12, "source.azimuth", {{0.0, -30.0}, {16.0, 45.0}}, 0, -1, {}},
                                  {2, Instance::Insert, 7, "mix", {}, 12, 3, "chords"},
                                  {3, Instance::Decoder, 1, "output.gain", {{4.0, -6.0}}, 0, -1, {}}};
    CHECK(lanesFrom(Json::parse(toJson(lanes).dump(0))) == lanes);
    const auto read = lanesFrom(Json::parse(R"([{"id": 4, "instance": "mixer", "owner": 1, "param": "x"},
                                                {"id": 5, "instance": "encoder", "owner": 1, "param": ""},
                                                {"id": 0, "instance": "encoder", "owner": 1, "param": "x"}])"));
    CHECK(read.empty());
}

TEST_CASE("a clip runs on from where Live said it was, looping in its loop, held at its end when it does not loop") {
    //  Catches: a loop that wraps to the clip's start rather than its loop's, a one-shot clip that wraps, time run
    //  backwards.
    const SlotPlay looping{1, 0, 3.0, 1.0, 5.0, true};
    CHECK(clipPosition(looping, 1.0) == 4.0);
    CHECK(clipPosition(looping, 2.5) == 1.5);
    CHECK(clipPosition(looping, 10.0) == 1.0 + std::fmod(12.0, 4.0));
    CHECK(clipPosition(looping, -1.0) == 3.0);
    const SlotPlay late{1, 0, 4.0, 3.0, 5.0, true};  // a loop that starts late in the clip
    CHECK(clipPosition(late, 1.5) == 3.5);
    const SlotPlay once{1, 0, 3.0, 0.0, 5.0, false};
    CHECK(clipPosition(once, 4.0) == 5.0);
}

TEST_CASE("a clip lane follows its clip when the scenes move, and keeps it when it is renamed where it stands") {
    //  Catches: a lane left on a slot that now holds another clip, moved to one of two clips of its name, or let go
    //  of a clip that was only renamed.
    Arrangement a;
    a.session = {
        {3, {{0, "intro", "#000000", 0.0, 4.0, true, false}, {2, "chords", "#000000", 0.0, 8.0, true, false}}}};
    std::vector<Lane> lanes{{1, Instance::Encoder, 3, "x", {}, 3, 0, "chords"},  // its scene moved down two
                            {2, Instance::Encoder, 3, "x", {}, 3, 0, "intro"},   // where it was
                            {3, Instance::Encoder, 3, "x", {}, 3, 2, "verse"}};  // renamed to "chords"
    CHECK(followClips(lanes, a));
    CHECK(lanes[0].slot == 2);
    CHECK(lanes[1].slot == 0);
    CHECK(lanes[2].slot == 2);
    CHECK(lanes[2].clipName == "chords");
    CHECK_FALSE(followClips(lanes, a));
    a.session[0].clips.push_back({3, "chords", "#000000", 0.0, 8.0, true, false});
    lanes[0].slot = 0;  // two clips of its name now: it stays
    followClips(lanes, a);
    CHECK(lanes[0].slot == 0);
}
