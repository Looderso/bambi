// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Which stereo pair each Live track plays on, as tracks come and go.

#include <algorithm>
#include <vector>

#include "bambi/live/pairs.hpp"
#include "doctest.h"

using namespace bambi::live;

namespace {

int pairOf(const PairAssignment& a, TrackId t) {
    const auto it = std::find_if(a.pairs.begin(), a.pairs.end(), [&](const TrackPair& p) { return p.track == t; });
    return it == a.pairs.end() ? -1 : it->pair;
}

}  // namespace

TEST_CASE("a fresh list takes the pairs in order, and the same list again changes nothing") {
    //  Catches: assigning from scratch on every call, which would reshuffle a set whose tracks moved.
    const std::vector<TrackId> tracks{40, 10, 30};
    const PairAssignment first = assignPairs({}, tracks, 8);
    CHECK(first.pairs == std::vector<TrackPair>{{40, 0}, {10, 1}, {30, 2}});
    CHECK(first.unassigned.empty());

    const std::vector<TrackId> moved{30, 40, 10};
    const PairAssignment again = assignPairs(first.pairs, moved, 8);
    CHECK(pairOf(again, 40) == 0);
    CHECK(pairOf(again, 10) == 1);
    CHECK(pairOf(again, 30) == 2);
}

TEST_CASE("a track keeps its pair when others are added, removed or reordered") {
    //  Catches: assigning by position in the list -- deleting the first track would move every other one.
    const PairAssignment before = assignPairs({}, std::vector<TrackId>{1, 2, 3, 4}, 8);
    const PairAssignment after = assignPairs(before.pairs, std::vector<TrackId>{4, 5, 3}, 8);
    CHECK(pairOf(after, 4) == 3);
    CHECK(pairOf(after, 3) == 2);
    CHECK(pairOf(after, 1) == -1);
    CHECK(pairOf(after, 2) == -1);
    CHECK(pairOf(after, 5) == 0);  // the lowest pair nobody holds: 1's
}

TEST_CASE("no pair is given twice, and new tracks fill the gaps from the lowest") {
    //  Catches: a free-pair search that starts past the gaps, or one that hands a held pair to a new track.
    const std::vector<TrackPair> previous{{1, 0}, {3, 2}, {6, 5}};
    const PairAssignment a = assignPairs(previous, std::vector<TrackId>{7, 1, 8, 3, 9, 6, 10}, 8);
    CHECK(pairOf(a, 1) == 0);
    CHECK(pairOf(a, 3) == 2);
    CHECK(pairOf(a, 6) == 5);
    CHECK(pairOf(a, 7) == 1);
    CHECK(pairOf(a, 8) == 3);
    CHECK(pairOf(a, 9) == 4);
    CHECK(pairOf(a, 10) == 6);
    std::vector<int> used;
    for (const TrackPair& p : a.pairs) used.push_back(p.pair);
    std::sort(used.begin(), used.end());
    CHECK(std::adjacent_find(used.begin(), used.end()) == used.end());
}

TEST_CASE("past the device's pairs a track is reported, never wrapped onto a pair in use") {
    //  Catches: a pair taken modulo the count, which would play two tracks through one encoder.
    const PairAssignment a = assignPairs({}, std::vector<TrackId>{1, 2, 3, 4}, 2);
    CHECK(a.pairs == std::vector<TrackPair>{{1, 0}, {2, 1}});
    CHECK(a.unassigned == std::vector<TrackId>{3, 4});

    //  and a pair that no longer exists on the device is given up, not kept
    const PairAssignment smaller = assignPairs(std::vector<TrackPair>{{9, 5}}, std::vector<TrackId>{9}, 2);
    CHECK(pairOf(smaller, 9) == 0);
}

TEST_CASE("a corrupt previous assignment or a repeated track still gives each pair once") {
    //  Catches: trusting the previous assignment, where two tracks claiming one pair would both get it.
    const std::vector<TrackPair> previous{{1, 0}, {2, 0}};
    const PairAssignment a = assignPairs(previous, std::vector<TrackId>{1, 2, 2}, 4);
    CHECK(a.pairs == std::vector<TrackPair>{{1, 0}, {2, 1}});
}
