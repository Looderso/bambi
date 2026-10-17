// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Where the routing page puts Live's tracks and groups.

#include <algorithm>

#include "bambi/live/page.hpp"
#include "doctest.h"

using namespace bambi::live;

namespace {

Node node(TrackId id, Node::Kind kind = Node::Kind::Track, std::optional<TrackId> parent = std::nullopt) {
    Node n;
    n.id = id;
    n.kind = kind;
    n.parent = parent;
    return n;
}

const Place& at(const PageLayout& l, TrackId id) {
    const auto p = std::find_if(l.places.begin(), l.places.end(), [&](const Place& x) { return x.node == id; });
    REQUIRE(p != l.places.end());
    return *p;
}

}  // namespace

TEST_CASE("a column per depth, the deepest on the left, Main right of the top level") {
    //  Catches: depth counted from the left, so a nested track would sit right of its group, or Main on a column
    //  of its own tracks.
    //  group 1 { track 2, group 3 { track 4 } }, track 5
    const Tree t{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}),
                  node(3, Node::Kind::Group, TrackId{1}), node(4, Node::Kind::Track, TrackId{3}), node(5)}};
    const PageLayout l = layOutPage(t);
    CHECK(at(l, 4).column == 0);
    CHECK(at(l, 2).column == 1);
    CHECK(at(l, 3).column == 1);
    CHECK(at(l, 1).column == 2);
    CHECK(at(l, 5).column == 2);
    CHECK(l.main.column == 3);
}

TEST_CASE("a row per track that holds nothing, in Live's order; a group midway between its first and last child") {
    //  Catches: a group given a row of its own beside its children, or rows out of Live's order.
    const Tree t{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}),
                  node(3, Node::Kind::Group, TrackId{1}), node(4, Node::Kind::Track, TrackId{3}),
                  node(6, Node::Kind::Track, TrackId{1}), node(5)}};
    const PageLayout l = layOutPage(t);
    CHECK(l.rows == 4);
    CHECK(at(l, 2).row == 0.0);
    CHECK(at(l, 4).row == 1.0);
    CHECK(at(l, 3).row == 1.0);  // its only child's row
    CHECK(at(l, 6).row == 2.0);
    CHECK(at(l, 1).row == 1.0);  // midway between 0 and 2
    CHECK(at(l, 5).row == 3.0);
    CHECK(l.main.row == 2.0);  // midway between the top level's first, 1, and last, 3
}

TEST_CASE("a group that holds nothing takes a row; returns are not on the page's tree") {
    //  Catches: an empty group with no row, drawn over the next track, or a return among the tracks.
    const Tree t{{node(1, Node::Kind::Group), node(2), node(9, Node::Kind::Return)}};
    const PageLayout l = layOutPage(t);
    CHECK(l.places.size() == 2);
    CHECK(at(l, 1).row == 0.0);
    CHECK(at(l, 2).row == 1.0);
    CHECK(l.rows == 2);
}

TEST_CASE("a track whose group the tree does not have stands at the top") {
    //  Catches: a track lost from the page because its group is missing from what the device sent.
    const PageLayout l = layOutPage(Tree{{node(2, Node::Kind::Track, TrackId{77})}});
    REQUIRE(l.places.size() == 1);
    CHECK(at(l, 2).column == 0);
    CHECK(l.main.column == 1);
}
