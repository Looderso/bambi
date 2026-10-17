// SPDX-License-Identifier: GPL-3.0-or-later
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
    CHECK(pointNear(l, 4.8, 0.1, 0.5, 0.2) == std::optional<std::size_t>{1});
    CHECK_FALSE(pointNear(l, 4.5, 0.5, 0.2, 0.2).has_value());
    CHECK_FALSE(pointNear(l, 4.0, 0.9, 0.5, 0.2).has_value());
}

TEST_CASE("the lanes read back as written, and one the set cannot read is left out") {
    //  Catches: a field lost between the set and the host, the instance read as another, a broken lane taken.
    const std::vector<Lane> lanes{{1, Instance::Encoder, 12, "source.azimuth", {{0.0, -30.0}, {16.0, 45.0}}},
                                  {2, Instance::Insert, 7, "mix", {}},
                                  {3, Instance::Decoder, 1, "output.level", {{4.0, -6.0}}}};
    CHECK(lanesFrom(Json::parse(toJson(lanes).dump(0))) == lanes);
    const auto read = lanesFrom(Json::parse(R"([{"id": 4, "instance": "mixer", "owner": 1, "param": "x"},
                                                {"id": 5, "instance": "encoder", "owner": 1, "param": ""}])"));
    CHECK(read.empty());
}
