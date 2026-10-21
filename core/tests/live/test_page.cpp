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

TEST_CASE("an Encoder added takes that node alone, at any depth; inside a group encoded whole, the group opens") {
    //  Catches: a track's siblings taken with it, its groups left out of the field, a node taken twice, or -- inside a
    //  group encoded whole -- the group left whole, which plays the track twice, or what else it held dropped to Live.
    Shape s;
    addEncoder(s, set(), 5);
    CHECK(s.taken == std::vector<TrackId>{5});
    addEncoder(s, set(), 4);  // inside group 3, inside group 1
    CHECK(s.taken == std::vector<TrackId>{5, 4});
    const Field f = layOut(set(), s);
    CHECK(roleOf(set(), f, 4) == Role::Source);
    CHECK(roleOf(set(), f, 2) == Role::Live);  // its group's other track stays in Live
    CHECK(roleOf(set(), f, 3) == Role::Bus);
    CHECK(roleOf(set(), f, 1) == Role::Bus);
    addEncoder(s, set(), 5);
    CHECK(s.taken == std::vector<TrackId>{5, 4});

    Shape whole;
    whole.taken = {1};
    addEncoder(whole, set(), 4);
    CHECK(whole.taken == std::vector<TrackId>{2, 4});  // 1 opened, 2 taken in its place; 3 opened on the way
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
    s.taken = {2, 3};
    const Field bus = layOut(set(), s);
    CHECK(roleOf(set(), bus, 1) == Role::Bus);
    CHECK(roleOf(set(), bus, 3) == Role::Source);
    CHECK(roleOf(set(), bus, 4) == Role::InGroup);
}

TEST_CASE("taking an Encoder away gives that node alone back to Live; on a bus, the group is taken whole") {
    //  Catches: a track's group or siblings given back with it, a bus taken out of the field rather than encoded
    //  whole, or a mark left inside a group taken whole.
    Shape s;
    s.taken = {2, 4, 5};
    removeEncoder(s, set(), layOut(set(), s), 2);
    CHECK(s.taken == std::vector<TrackId>{4, 5});  // 1 stays a bus, of 3's, of 4
    CHECK(roleOf(set(), layOut(set(), s), 1) == Role::Bus);
    removeEncoder(s, set(), layOut(set(), s), 1);  // "encode whole"
    CHECK(s.taken == std::vector<TrackId>{5, 1});
    removeEncoder(s, set(), layOut(set(), s), 1);
    CHECK(s.taken == std::vector<TrackId>{5});
}

TEST_CASE("a group given back to Live takes nothing hidden inside it along") {
    //  Catches: marks left under a group taken whole coming back as sources the moment the group is given back.
    Shape s;
    s.taken = {1, 4};  // 4 is inside 1, which is encoded whole: its mark means nothing
    removeEncoder(s, set(), layOut(set(), s), 1);
    CHECK(s.taken.empty());
    CHECK(layOut(set(), s).parts.empty());
}

//  A group 1 holding track 2, which plays into it, and track 3, filed under it in Live and routed by hand to Main.
Tree handRouted() {
    Node playing = node(2, Node::Kind::Track, TrackId{1});
    playing.group = TrackId{1};
    Node past = node(3, Node::Kind::Track);  // plays to Main
    past.group = TrackId{1};
    return Tree{{node(1, Node::Kind::Group), playing, past}};
}

TEST_CASE("a member routed past its group is listed under it, as Live files it") {
    //  Catches: the page nesting by where audio goes, which lists a member routed to Main at the top and the page no
    //  longer looks like Live's.
    const auto lines = linesOf(handRouted());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == Line{1, 0});
    CHECK(lines[1] == Line{2, 1});
    CHECK(lines[2] == Line{3, 1});
    //  and a device that sends no group files a node where it plays, as before
    const auto old = linesOf(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1})}});
    REQUIRE(old.size() == 2);
    CHECK(old[1] == Line{2, 1});
}

TEST_CASE("what a member is follows where its audio goes: one routed past its group is its own path") {
    /*  Catches: a hand-routed member read as inside the group the group encodes -- it would never be heard in the field,
        since its audio does not reach the group -- or an Encoder on it opening the group it does not play into, or one
        on a member that does play into it leaving the group whole, which plays that member twice -- or a member
        routed elsewhere from inside the group taken along when it opens. */
    const Tree t = handRouted();
    Shape s;
    s.taken = {1};
    const Field whole = layOut(t, s);
    CHECK(roleOf(t, whole, 2) == Role::InGroup);
    CHECK(roleOf(t, whole, 3) == Role::Live);

    Shape own = s;
    addEncoder(own, t, 3);
    CHECK(own.taken == std::vector<TrackId>{1, 3});

    Shape member = s;
    addEncoder(member, t, 2);
    CHECK(member.taken == std::vector<TrackId>{2});
    CHECK(roleOf(t, layOut(t, member), 1) == Role::Bus);

    //  and the group opened round a member leaves out one routed elsewhere from inside it
    Tree away = t;
    Node elsewhere = node(4, Node::Kind::Track, TrackId{1});
    elsewhere.path = Node::Path::Elsewhere;
    away.nodes.push_back(elsewhere);
    Shape opened = s;
    addEncoder(opened, away, 2);
    CHECK(opened.taken == std::vector<TrackId>{2});
}
