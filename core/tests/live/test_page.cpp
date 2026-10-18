// SPDX-License-Identifier: GPL-3.0-or-later
//
//  What the routing page shows of Live's tracks and groups, and what adding or taking away an Encoder does.

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

//  A group 1 holding a track 2 and a group 3, which holds a track 4; a track 5; a return 6.
Tree set() {
    return Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}),
                 node(3, Node::Kind::Group, TrackId{1}), node(4, Node::Kind::Track, TrackId{3}), node(5),
                 node(6, Node::Kind::Return)}};
}

}  // namespace

TEST_CASE("the page lists Live's order, a group before what it holds, each a line deeper; no returns") {
    //  Catches: a child listed before its group or under another, a depth off by one, a return among the tracks, a
    //  node whose group is not in the tree lost.
    CHECK(linesOf(set()) == std::vector<Line>{{1, 0}, {2, 1}, {3, 1}, {4, 2}, {5, 0}});
    CHECK(linesOf(Tree{{node(2, Node::Kind::Track, TrackId{77})}}) == std::vector<Line>{{2, 0}});
}

TEST_CASE("an Encoder added in Live takes the node at the top, or splits every group down to one inside") {
    //  Catches: a track inside a group taken on its own -- it is no node at the top, so nothing would play -- a group
    //  between left whole, or a node already taken taken twice.
    Shape s;
    addEncoder(s, set(), 5);
    CHECK(s.taken == std::vector<TrackId>{5});
    addEncoder(s, set(), 4);
    CHECK(s.taken == std::vector<TrackId>{5, 1});
    CHECK(s.split == std::vector<TrackId>{3, 1});
    const Field f = layOut(set(), s);
    CHECK(roleOf(set(), f, 4) == Role::Source);
    CHECK(roleOf(set(), f, 2) == Role::Source);  // its siblings with it
    CHECK(roleOf(set(), f, 3) == Role::Bus);
    addEncoder(s, set(), 5);
    CHECK(s.taken == std::vector<TrackId>{5, 1});
}

TEST_CASE("each node's role: in Live, a source, a bus, or inside a group encoded whole") {
    //  Catches: a child of a whole group read as in Live -- the page would offer to take it, playing it twice -- or
    //  a bus read as a source.
    Shape s;
    s.taken = {1};
    const Field whole = layOut(set(), s);
    CHECK(roleOf(set(), whole, 1) == Role::Source);
    CHECK(roleOf(set(), whole, 4) == Role::InGroup);
    CHECK(roleOf(set(), whole, 5) == Role::Live);
    s.split = {1};
    const Field split = layOut(set(), s);
    CHECK(roleOf(set(), split, 1) == Role::Bus);
    CHECK(roleOf(set(), split, 3) == Role::Source);
    CHECK(roleOf(set(), split, 4) == Role::InGroup);
}

TEST_CASE("taking an Encoder away gives a node at the top back to Live, and encodes a bus whole again") {
    //  Catches: a bus taken out of the field rather than encoded whole, or a source left in the field.
    Shape s;
    s.taken = {1, 5};
    s.split = {1};
    removeEncoder(s, set(), layOut(set(), s), 1);
    CHECK(s.split.empty());
    CHECK(s.taken == std::vector<TrackId>{1, 5});
    removeEncoder(s, set(), layOut(set(), s), 5);
    CHECK(s.taken == std::vector<TrackId>{1});
}
