// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Which of a Live set's tracks and groups the field takes, and as what.

#include "bambi/live/field.hpp"
#include "doctest.h"

using namespace bambi;
using namespace bambi::live;

namespace {

Node node(TrackId id, Node::Kind kind = Node::Kind::Track, std::optional<TrackId> parent = std::nullopt) {
    Node n;
    n.id = id;
    n.kind = kind;
    n.parent = parent;
    return n;
}

//  A group 1 holding a track 2 and a group 3, which holds a track 4; a track 5 at the top; a return 6.
Tree set() {
    return Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}),
                 node(3, Node::Kind::Group, TrackId{1}), node(4, Node::Kind::Track, TrackId{3}), node(5),
                 node(6, Node::Kind::Return)}};
}

}  // namespace

TEST_CASE("unshaped, every node at the top is a source, a group with all it holds") {
    //  Catches: a group's children encoded beside the group, which would play them twice, or a return taken.
    const Field f = layOut(set(), {});
    CHECK(f.sources() == std::vector<TrackId>{1, 5});
    for (const Part& p : f.parts) CHECK_FALSE(p.bus);
}

TEST_CASE("a split group is a bus, and what plays into it is laid out under it") {
    //  Catches: the bus left out, a child under the wrong bus, or a bus after what plays into it.
    const Field f = layOut(set(), Shape{{1}, {}, {}, {}, {}});
    REQUIRE(f.parts.size() == 4);
    CHECK(f.parts[0] == Part{1, true, -1, {}, 0});
    CHECK(f.parts[1] == Part{2, false, 0, {}, 0});
    CHECK(f.parts[2] == Part{3, false, 0, {}, 0});  // a group not split: a source, with what it holds
    CHECK(f.parts[3] == Part{5, false, -1, {}, 0});
}

TEST_CASE("a group split inside a split group is a bus inside the bus") {
    //  Catches: nested groups flattened onto the outer bus, or the inner bus kept as a source too.
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}});
    CHECK(f.sources() == std::vector<TrackId>{2, 4, 5});
    REQUIRE(f.parts.size() == 5);
    CHECK(f.parts[2] == Part{3, true, 0, {}, 0});
    CHECK(f.parts[3] == Part{4, false, 2, {}, 0});
}

TEST_CASE("a node at the top left in Live is not in the field; below the top the mark means nothing") {
    //  Catches: a node left in Live still given a pair, or a child of a split group dropped by a mark it ignores.
    const Field f = layOut(set(), Shape{{1}, {5, 2}, {}, {}, {}});
    CHECK(f.sources() == std::vector<TrackId>{2, 3});
}

TEST_CASE("under a split group, only what plays into it with audio; an empty bus is not in the field") {
    //  Catches: a child that plays elsewhere or has no audio given a pair, or a bus with no sources left in the
    //  plan, which would sum nothing and still cost its buffer.
    Tree t;
    t.nodes.push_back(node(1, Node::Kind::Group));
    Node away = node(2, Node::Kind::Track, TrackId{1});
    away.path = Node::Path::Elsewhere;
    t.nodes.push_back(away);
    Node quiet = node(3, Node::Kind::Track, TrackId{1});
    quiet.audio = false;
    t.nodes.push_back(quiet);
    t.nodes.push_back(node(4));
    const Field f = layOut(t, Shape{{1}, {}, {}, {}, {}});
    REQUIRE(f.parts.size() == 1);
    CHECK(f.parts[0] == Part{4, false, -1, {}, 0});
}

TEST_CASE("split on a track means nothing") {
    //  Catches: a track made a bus with nothing in it, which would take it out of the field.
    CHECK(layOut(set(), Shape{{5}, {}, {}, {}, {}}).sources() == std::vector<TrackId>{1, 5});
}

TEST_CASE("the shape the set keeps reads back as it was, and nonsense in it is left out") {
    //  Catches: a list written under one key and read under another, or an id that is not one read as 0.
    const Shape shape{{3, 1}, {5}, {}, {}, {}};
    CHECK(shapeFrom(toJson(shape)) == shape);
    Json odd = toJson(shape);
    Json split = Json::array();
    split.push(Json("x"));
    split.push(Json(0.0));
    split.push(Json(7.0));
    odd.set("split", split);
    CHECK(shapeFrom(odd).split == std::vector<TrackId>{7});
}

TEST_CASE("the plan for a field: a node per source with a pair, its own Encoder, and every bus") {
    //  Catches: a source encoded from another's pair, two sources on one Encoder slot, a bus's child left
    //  under Main, or a decoder slot that is an Encoder's.
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}});  // bus 1 { 2, bus 3 { 4 } }, 5
    const FieldPlan fp = planOf(f, {{2, 4}, {4, 0}, {5, 6}});
    REQUIRE(fp.plan.nodes.size() == 5);
    CHECK(fp.encoders == std::vector<TrackId>{2, 4, 5});
    CHECK(fp.plan.nodes[0].pair == -1);
    CHECK(fp.plan.nodes[1].pair == 4);
    CHECK(fp.plan.nodes[1].encoder == 0);
    CHECK(fp.plan.nodes[1].parent == 0);
    CHECK(fp.plan.nodes[3].parent == 2);
    CHECK(fp.plan.nodes[3].encoder == 1);
    CHECK(fp.plan.nodes[4].parent == -1);
    CHECK(fp.plan.decoder == 3);
    CHECK(fp.plan.slots.size() == 4);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("a source without a pair is left out of the plan, and the parts after it keep their buses") {
    //  Catches: a node with no pair handed to the runner, or a child pointing at the wrong node once one is
    //  skipped before its bus.
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}});
    const FieldPlan fp = planOf(f, {{4, 1}, {5, 2}});  // track 2 got none
    REQUIRE(fp.plan.nodes.size() == 4);
    CHECK(fp.encoders == std::vector<TrackId>{4, 5});
    CHECK(fp.plan.nodes[1].track == 3);
    CHECK(fp.plan.nodes[2].track == 4);
    CHECK(fp.plan.nodes[2].parent == 1);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("a node's chain goes with its part, Main's with the field, and one on a node not in the field nowhere") {
    //  Catches: a chain on the wrong part, Main's lost, or a chain on a node left in Live played anyway.
    const Shape shape{{1},
                      {5},
                      {{2, {{7, Effect::Echo, 0}}}, {kMain, {{8, Effect::Reverb, 0}}}, {5, {{9, Effect::Echo, 0}}}},
                      {},
                      {}};
    const Field f = layOut(set(), shape);
    REQUIRE(f.parts.size() == 3);
    CHECK(f.parts[1].node == 2);
    CHECK(f.parts[1].inserts == std::vector<Insert>{{7, Effect::Echo, 0}});
    CHECK(f.parts[0].inserts.empty());
    CHECK(f.main == std::vector<Insert>{{8, Effect::Reverb, 0}});
}

TEST_CASE("the plan's insert slots follow the Encoders, the parts' in order and Main's last, the decoder after") {
    //  Catches: an insert on an Encoder's slot, two inserts on one slot, Main's chain in a part, or an insert of a
    //  source without a pair still given a slot -- its plugin would be loaded and never played.
    const Shape shape{{1},
                      {},
                      {{1, {{10, Effect::Reverb, 0}}},
                       {2, {{11, Effect::Echo, 0}, {12, Effect::Reverb, 0}}},
                       {3, {{13, Effect::Echo, 0}}},
                       {kMain, {{14, Effect::Echo, 0}}}},
                      {},
                      {}};
    const Field f = layOut(set(), shape);              // bus 1 { 2, 3 }, 5
    const FieldPlan fp = planOf(f, {{2, 0}, {5, 1}});  // 3 got no pair
    CHECK(fp.encoders == std::vector<TrackId>{2, 5});
    CHECK(fp.effects ==
          std::vector<Insert>{
              {10, Effect::Reverb, 0}, {11, Effect::Echo, 0}, {12, Effect::Reverb, 0}, {14, Effect::Echo, 0}});
    REQUIRE(fp.plan.nodes.size() == 3);
    CHECK(fp.plan.nodes[0].inserts == std::vector<int>{2});
    CHECK(fp.plan.nodes[1].inserts == std::vector<int>{3, 4});
    CHECK(fp.plan.nodes[2].inserts.empty());
    CHECK(fp.plan.main == std::vector<int>{5});
    CHECK(fp.plan.decoder == 6);
    CHECK(fp.plan.slots.size() == 7);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("the chains the set keeps read back as they were, and an insert that is not one is left out") {
    //  Catches: Main's chain lost because its node is 0, an insert's effect read as the other, or an insert with no
    //  id or an unknown effect read in.
    const Shape shape{
        {}, {}, {{kMain, {{3, Effect::Reverb, 0}}}, {4, {{5, Effect::Echo, 0}, {6, Effect::Reverb, 0}}}}, {}, {}};
    CHECK(shapeFrom(toJson(shape)) == shape);
    const auto read = shapeFrom(Json::parse(R"({"chains":[{"node":4,"inserts":[{"id":0,"effect":"echo"},
        {"id":7,"effect":"phaser"},{"id":8,"effect":"echo"}]},{"inserts":[]}]})"));
    REQUIRE(read.chains.size() == 1);
    CHECK(read.chains[0].inserts == std::vector<Insert>{{8, Effect::Echo, 0}});
}

namespace {

//  A group 1 holding a track 2, a track 3; Live's returns A (10) and B (11). Each send knob in dB, A first.
Tree sending() {
    Tree t;
    Node g = node(1, Node::Kind::Group);
    g.sends = {-6.0, std::nullopt};
    Node a = node(2, Node::Kind::Track, TrackId{1});
    a.sends = {0.0, -20.0};
    Node b = node(3);
    b.sends = {-12.0};  // no knob for B
    t.nodes = {g, a, b, node(10, Node::Kind::Return), node(11, Node::Kind::Return)};
    return t;
}

}  // namespace

TEST_CASE("a return the shape takes is a return of the field, fed by its own knob, with its own chain") {
    //  Catches: a return fed by the knob of its place in the field rather than in Live, its chain lost, or a
    //  track's id taken as a return.
    const Shape shape{{}, {}, {{11, {{20, Effect::Reverb, 0}}}}, {11, 3}, {}};
    const Field f = layOut(sending(), shape);
    REQUIRE(f.returns.size() == 1);
    CHECK(f.returns[0].node == 11);
    CHECK(f.returns[0].send == 1);
    CHECK(f.returns[0].inserts == std::vector<Insert>{{20, Effect::Reverb, 0}});
}

TEST_CASE("a return's chain is slotted after the parts' inserts and before Main's") {
    //  Catches: a return with no Return in the plan, or its chain on a slot of a part's or Main's insert.
    const Shape shape{{1},
                      {},
                      {{2, {{5, Effect::Echo, 0}}}, {10, {{6, Effect::Reverb, 0}}}, {kMain, {{7, Effect::Echo, 0}}}},
                      {10},
                      {}};
    const Field f = layOut(sending(), shape);
    const FieldPlan fp = planOf(f, {{2, 0}, {3, 1}});
    CHECK(fp.encoders.size() == 2);
    CHECK(fp.effects == std::vector<Insert>{{5, Effect::Echo, 0}, {6, Effect::Reverb, 0}, {7, Effect::Echo, 0}});
    REQUIRE(fp.plan.returns.size() == 1);
    CHECK(fp.plan.returns[0].chain == std::vector<int>{3});
    CHECK(fp.plan.main == std::vector<int>{4});
    CHECK(fp.plan.decoder == 5);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("every send's level is its node's own knob in Live, as a gain, by node and return") {
    //  Catches: dB taken as a gain, a level on another node's or another return's entry, a bus without its group's
    //  knob, or a missing or silent knob heard at some level.
    const Shape shape{{1}, {}, {}, {10, 11}, {}};
    const Field f = layOut(sending(), shape);  // bus 1 { 2 }, 3; returns A, B
    const FieldPlan fp = planOf(f, {{2, 0}, {3, 1}});
    const auto levels = sendLevels(fp, f, sending());
    REQUIRE(levels.size() == 6);
    CHECK(levels[0] == doctest::Approx(0.501187));  // the group to A, -6 dB
    CHECK(levels[1] == 0.0f);                       // the group to B: silence
    CHECK(levels[2] == doctest::Approx(1.0));       // track 2 to A, 0 dB
    CHECK(levels[3] == doctest::Approx(0.1));       // track 2 to B, -20 dB
    CHECK(levels[4] == doctest::Approx(0.251189));  // track 3 to A, -12 dB
    CHECK(levels[5] == 0.0f);                       // track 3 has no knob for B
}

TEST_CASE("the returns the set keeps read back") {
    //  Catches: the field's returns written but never read, so a reopened set plays them in Live again.
    const Shape shape{{}, {}, {}, {11, 10}, {}};
    CHECK(shapeFrom(toJson(shape)).returns == std::vector<TrackId>{11, 10});
}

TEST_CASE("an insert's sidechain hears its source's pair, and silence once the source has none") {
    //  Catches: a sidechain on another track's pair, on a bus's or a node's left in Live -- neither arrives on a
    //  pair of its own -- or on a source the loop device had no pair for.
    const Shape shape{
        {1},
        {5},
        {{2, {{7, Effect::Reverb, 3}, {8, Effect::Echo, 5}, {9, Effect::Echo, 1}, {10, Effect::Echo, 4}}}},
        {},
        {}};
    const Field f = layOut(set(), shape);              // bus 1 { 2, 3 }; 5 in Live
    const FieldPlan fp = planOf(f, {{2, 0}, {3, 6}});  // track 4 is in group 3, encoded whole with it
    REQUIRE(fp.effects.size() == 4);
    const auto heard = [&](std::size_t e) { return fp.plan.slots[fp.encoders.size() + e].sidechain; };
    CHECK(heard(0) == 6);                     // track 3's pair
    CHECK(heard(1) == -1);                    // track 5 stays in Live
    CHECK(heard(2) == -1);                    // 1 is a bus
    CHECK(heard(3) == -1);                    // 4 plays inside group 3, which the field takes whole
    CHECK(fp.plan.slots[0].sidechain == -1);  // an Encoder's slot
}

TEST_CASE("a sidechain the set keeps reads back, and an insert without one has none") {
    //  Catches: a sidechain written but not read, or one read where none was written.
    const Shape shape{{}, {}, {{2, {{7, Effect::Reverb, 3}, {8, Effect::Echo, 0}}}}, {}, {}};
    const Shape back = shapeFrom(toJson(shape));
    REQUIRE(back.chains.size() == 1);
    CHECK(back.chains[0].inserts[0].sidechain == 3);
    CHECK(back.chains[0].inserts[1].sidechain == 0);
}

TEST_CASE("a source's Encoder hears the pair of the track its sidechain names; never itself, silence without a pair") {
    //  Catches: an Encoder's sidechain on its own pair, on another source's, or on a pair when its track has none;
    //  or a source that names itself hearing itself.
    const Shape shape{{1}, {}, {}, {}, {{2, 3}, {3, 3}, {5, 4}}};
    const Field f = layOut(set(), shape);  // bus 1 { 2, 3 }, 5; group 3 holds 4
    const FieldPlan fp = planOf(f, {{2, 0}, {3, 6}, {5, 1}});
    REQUIRE(fp.encoders == std::vector<TrackId>{2, 3, 5});
    CHECK(fp.plan.slots[0].sidechain == 6);   // 2 hears 3
    CHECK(fp.plan.slots[1].sidechain == -1);  // 3 named itself
    CHECK(fp.plan.slots[2].sidechain == -1);  // 5 hears 4, which has no pair of its own
    CHECK(fp.plan.slots[3].sidechain == -1);  // the decoder
}

TEST_CASE("the Encoders' sidechains the set keeps read back") {
    //  Catches: a source's sidechain written but not read, or its two tracks swapped.
    const Shape shape{{}, {}, {}, {}, {{2, 3}, {5, 2}}};
    CHECK(shapeFrom(toJson(shape)).sidechains == shape.sidechains);
}
