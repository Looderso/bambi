// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Which of a Live set's tracks and groups the field takes, and as what.

#include <random>
#include <string>

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
    const Field f = layOut(set(), Shape{{1}, {}, {}, {}, {}, {}, {}, {}});
    REQUIRE(f.parts.size() == 4);
    CHECK(f.parts[0] == Part{1, true, -1, {}, 0, {{Target::main(), 0.0}}});
    CHECK(f.parts[1] == Part{2, false, 0, {}, 0, {{Target::node(1), 0.0}}});  // into its bus
    CHECK(f.parts[2] ==
          Part{3, false, 0, {}, 0, {{Target::node(1), 0.0}}});  // a group not split: a source, with what it holds
    CHECK(f.parts[3] == Part{5, false, -1, {}, 0, {{Target::main(), 0.0}}});
}

TEST_CASE("a group split inside a split group is a bus inside the bus") {
    //  Catches: nested groups flattened onto the outer bus, or the inner bus kept as a source too.
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}, {}, {}, {}});
    CHECK(f.sources() == std::vector<TrackId>{2, 4, 5});
    REQUIRE(f.parts.size() == 5);
    CHECK(f.parts[2] == Part{3, true, 0, {}, 0, {{Target::node(1), 0.0}}});
    CHECK(f.parts[3] == Part{4, false, 2, {}, 0, {{Target::node(3), 0.0}}});
}

TEST_CASE("a node at the top left in Live is not in the field; below the top the mark means nothing") {
    //  Catches: a node left in Live still given a pair, or a child of a split group dropped by a mark it ignores.
    const Field f = layOut(set(), Shape{{1}, {5, 2}, {}, {}, {}, {}, {}, {}});
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
    const Field f = layOut(t, Shape{{1}, {}, {}, {}, {}, {}, {}, {}});
    REQUIRE(f.parts.size() == 1);
    CHECK(f.parts[0] == Part{4, false, -1, {}, 0, {{Target::main(), 0.0}}});
}

TEST_CASE("split on a track means nothing") {
    //  Catches: a track made a bus with nothing in it, which would take it out of the field.
    CHECK(layOut(set(), Shape{{5}, {}, {}, {}, {}, {}, {}, {}}).sources() == std::vector<TrackId>{1, 5});
}

TEST_CASE("the shape the set keeps reads back as it was, and nonsense in it is left out") {
    //  Catches: a list written under one key and read under another, or an id that is not one read as 0.
    const Shape shape{{3, 1}, {5}, {}, {}, {}, {}, {}, {}};
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
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}, {}, {}, {}});  // bus 1 { 2, bus 3 { 4 } }, 5
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 4}, {4, 0}, {5, 6}}));
    REQUIRE(fp.plan.nodes.size() == 5);
    CHECK(fp.encoders == std::vector<TrackId>{2, 4, 5});
    CHECK(fp.plan.nodes[0].input == -1);
    CHECK(fp.plan.nodes[1].input == 10);
    CHECK(fp.plan.nodes[1].encoder == 0);
    CHECK(fp.plan.nodes[1].wires == std::vector<live::PlanWire>{{PlanWire::To::Node, 0}});
    CHECK(fp.plan.nodes[3].wires == std::vector<live::PlanWire>{{PlanWire::To::Node, 2}});
    CHECK(fp.plan.nodes[3].encoder == 1);
    CHECK(fp.plan.nodes[4].wires == std::vector<live::PlanWire>{{PlanWire::To::Main, 0}});
    CHECK(fp.plan.decoders == std::vector<int>{3});
    CHECK(fp.plan.slots.size() == 4);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("a source without a pair is left out of the plan, and the parts after it keep their buses") {
    //  Catches: a node with no pair handed to the runner, or a child pointing at the wrong node once one is
    //  skipped before its bus.
    const Field f = layOut(set(), Shape{{1, 3}, {}, {}, {}, {}, {}, {}, {}});
    const FieldPlan fp = planOf(f, arrivalsOf({{4, 1}, {5, 2}}));  // track 2 got none
    REQUIRE(fp.plan.nodes.size() == 4);
    CHECK(fp.encoders == std::vector<TrackId>{4, 5});
    CHECK(fp.plan.nodes[1].track == 3);
    CHECK(fp.plan.nodes[2].track == 4);
    CHECK(fp.plan.nodes[2].wires == std::vector<live::PlanWire>{{PlanWire::To::Node, 1}});  // its bus, after the skip
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("a node's chain goes with its part, Main's with the field, and one on a node not in the field nowhere") {
    //  Catches: a chain on the wrong part, Main's lost, or a chain on a node left in Live played anyway.
    const Shape shape{
        {1}, {5}, {{2, {{7, Effect::Echo, 0}}}, {kMain, {{8, Effect::Reverb, 0}}}, {5, {{9, Effect::Echo, 0}}}},
        {},  {},  {},
        {},  {}};
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
                      {},
                      {},
                      {},
                      {}};
    const Field f = layOut(set(), shape);                          // bus 1 { 2, 3 }, 5
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {5, 1}}));  // 3 got no pair
    CHECK(fp.encoders == std::vector<TrackId>{2, 5});
    CHECK(fp.effects ==
          std::vector<Insert>{
              {10, Effect::Reverb, 0}, {11, Effect::Echo, 0}, {12, Effect::Reverb, 0}, {14, Effect::Echo, 0}});
    REQUIRE(fp.plan.nodes.size() == 3);
    CHECK(fp.plan.nodes[0].inserts == std::vector<int>{2});
    CHECK(fp.plan.nodes[1].inserts == std::vector<int>{3, 4});
    CHECK(fp.plan.nodes[2].inserts.empty());
    CHECK(fp.plan.main == std::vector<int>{5});
    CHECK(fp.plan.decoders == std::vector<int>{6});
    CHECK(fp.plan.slots.size() == 7);
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("the chains the set keeps read back as they were, and an insert that is not one is left out") {
    //  Catches: Main's chain lost because its node is 0, an insert's effect read as the other, or an insert with no
    //  id or an unknown effect read in.
    const Shape shape{{}, {}, {{kMain, {{3, Effect::Reverb, 0}}}, {4, {{5, Effect::Echo, 0}, {6, Effect::Reverb, 0}}}},
                      {}, {}, {},
                      {}, {}};
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
    g.fader = 0.0;
    Node a = node(2, Node::Kind::Track, TrackId{1});
    a.sends = {0.0, -20.0};
    Node b = node(3);
    b.sends = {-12.0};  // no knob for B
    t.nodes = {g, a, b, node(10, Node::Kind::Return), node(11, Node::Kind::Return)};
    return t;
}

}  // namespace

TEST_CASE("a return the shape takes is a return of the field, at its place in Live, with its own chain") {
    //  Catches: a return given its place in the field rather than in Live, its chain lost, or a track's id taken as
    //  a return.
    const Shape shape{{}, {}, {{11, {{20, Effect::Reverb, 0}}}}, {11, 3}, {}, {}, {}, {}};
    const Field f = layOut(sending(), shape);
    REQUIRE(f.returns.size() == 1);
    CHECK(f.returns[0].node == 11);
    CHECK(f.returns[0].inserts == std::vector<Insert>{{20, Effect::Reverb, 0}});
}

TEST_CASE("a return's chain is slotted after the parts' inserts and before Main's") {
    //  Catches: a return with no Return in the plan, or its chain on a slot of a part's or Main's insert.
    const Shape shape{
        {1},  {}, {{2, {{5, Effect::Echo, 0}}}, {10, {{6, Effect::Reverb, 0}}}, {kMain, {{7, Effect::Echo, 0}}}},
        {10}, {}, {},
        {},   {}};
    const Field f = layOut(sending(), shape);
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {3, 1}}));
    CHECK(fp.encoders.size() == 2);
    CHECK(fp.effects == std::vector<Insert>{{5, Effect::Echo, 0}, {6, Effect::Reverb, 0}, {7, Effect::Echo, 0}});
    REQUIRE(fp.plan.returns.size() == 1);
    CHECK(fp.plan.returns[0].chain == std::vector<int>{3});
    CHECK(fp.plan.main == std::vector<int>{4});
    CHECK(fp.plan.decoders == std::vector<int>{5});
    CHECK(compile(fp.plan).has_value());
}

TEST_CASE("the plan plays the field's wires, each by index, every level in the order of the plan's wires") {
    //  Catches: a wire to a bus, a return or a Decoder given another's index; a level on another wire's entry; dB
    //  taken as a gain; a wire to something the plan does not have kept with a level, which shifts every one after.
    Tree t = sending();  // group 1 { 2 }, 3; returns A (10), B (11)
    t.nodes[3].fader = 0.0;
    t.nodes[4].fader = 0.0;
    Shape shape{{1}, {}, {}, {10, 11}, {}, {}, {1, 2}, {}};
    shape.routing = {{2, {{Target::node(1), 0.0}, {Target::node(10), -6.0}}},
                     {3, {{Target::main(), 0.0}, {Target::node(11), -20.0}, {Target::decoder(2), -12.0}}},
                     {11, {{Target::decoder(2), -3.0}}}};
    const Field f = layOut(t, shape);
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {3, 1}}));
    REQUIRE(fp.plan.nodes.size() == 3);  // the bus, 2, 3
    CHECK(fp.plan.nodes[1].wires == std::vector<PlanWire>{{PlanWire::To::Node, 0}, {PlanWire::To::Return, 0}});
    CHECK(fp.plan.nodes[2].wires ==
          std::vector<PlanWire>{{PlanWire::To::Main, 0}, {PlanWire::To::Return, 1}, {PlanWire::To::Decoder, 1}});
    CHECK(fp.plan.returns[1].wires == std::vector<PlanWire>{{PlanWire::To::Decoder, 1}});
    CHECK(fp.plan.mainWires == std::vector<PlanWire>{{PlanWire::To::Decoder, 0}, {PlanWire::To::Decoder, 1}});
    const auto l = live::levels(fp, f, t);
    REQUIRE(l.size() == wiresOf(fp.plan).size());
    //  the bus to Main; 2 to the bus and A; 3 to Main, B and the second Decoder; A to Main; B to it; Main to both
    const std::vector<double> want{1.0, 1.0, 0.501187, 1.0, 0.1, 0.251189, 1.0, 0.707946, 1.0, 1.0};
    REQUIRE(l.size() == want.size());
    for (std::size_t e = 0; e < want.size(); ++e) CHECK(l[e] == doctest::Approx(want[e]));
    CHECK(compile(fp.plan).has_value());
    //  a source with no pair is left out with its wires; nothing after them moves
    const FieldPlan without3 = planOf(f, arrivalsOf({{2, 0}}));
    CHECK(live::levels(without3, f, t).size() == l.size() - 3);
    //  a level changed in the shape is heard with the plan as it was: no new plan for a level
    Shape louder = shape;
    louder.routing[0].wires[1].db = 0.0;
    CHECK(live::levels(fp, layOut(t, louder), t)[2] == doctest::Approx(1.0));
}

TEST_CASE("whatever wires a shape asks for, the field plays only wires it allows, and its plan holds together") {
    //  Catches: a wire laid out that canWire refuses on the field as laid out -- a loop let through by the order the
    //  nodes are wired in, a return into a bus -- which compile() would refuse as a cycle or a wire not there.
    std::mt19937 random(31);
    std::uniform_int_distribution<int> pick(0, 9), coin(0, 1);
    const std::vector<TrackId> nodes{1, 2, 3, 4, 5, 6, kMain};
    for (int round = 0; round < 500; ++round) {
        Shape s{{1, 3}, {}, {}, {6}, {}, {}, {1, 2}, {}};
        for (const TrackId from : nodes) {
            if (coin(random)) continue;
            Routing r{from, {}};
            for (int k = pick(random) % 4; k > 0; --k) {
                const int t = pick(random);
                r.wires.push_back({t < 6   ? Target::node(static_cast<TrackId>(t + 1))
                                   : t < 8 ? Target::decoder(static_cast<std::uint64_t>(t - 5))
                                           : Target::main(),
                                   0.0});
            }
            s.routing.push_back(r);
        }
        const Field f = layOut(set(), s);
        for (const TrackId from : nodes)
            for (const Wire& w : f.wiresOf(from)) CHECK(canWire(f, from, w.to));
        const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {4, 1}, {5, 2}}));
        std::string error;
        CHECK_MESSAGE(compile(fp.plan, &error).has_value(), error);
    }
}

TEST_CASE("the returns the set keeps read back") {
    //  Catches: the field's returns written but never read, so a reopened set plays them in Live again.
    const Shape shape{{}, {}, {}, {11, 10}, {}, {}, {}, {}};
    CHECK(shapeFrom(toJson(shape)).returns == std::vector<TrackId>{11, 10});
}

TEST_CASE("an insert's sidechain hears its source's pair, and silence once the source has none") {
    //  Catches: a sidechain on another track's pair, on a bus's or a node's left in Live -- neither arrives on a
    //  pair of its own -- or on a source the loop device had no pair for.
    const Shape shape{
        {1}, {5}, {{2, {{7, Effect::Reverb, 3}, {8, Effect::Echo, 5}, {9, Effect::Echo, 1}, {10, Effect::Echo, 4}}}},
        {},  {},  {},
        {},  {}};
    const Field f = layOut(set(), shape);                          // bus 1 { 2, 3 }; 5 in Live
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {3, 6}}));  // track 4 is in group 3, encoded whole with it
    REQUIRE(fp.effects.size() == 4);
    const auto heard = [&](std::size_t e) { return fp.plan.slots[fp.encoders.size() + e].sidechain; };
    CHECK(heard(0) == 14);                    // track 3's pair, 6, after Main's two
    CHECK(heard(1) == -1);                    // track 5 stays in Live
    CHECK(heard(2) == -1);                    // 1 is a bus
    CHECK(heard(3) == -1);                    // 4 plays inside group 3, which the field takes whole
    CHECK(fp.plan.slots[0].sidechain == -1);  // an Encoder's slot
}

TEST_CASE("a sidechain the set keeps reads back, and an insert without one has none") {
    //  Catches: a sidechain written but not read, or one read where none was written.
    const Shape shape{{}, {}, {{2, {{7, Effect::Reverb, 3}, {8, Effect::Echo, 0}}}}, {}, {}, {}, {}, {}};
    const Shape back = shapeFrom(toJson(shape));
    REQUIRE(back.chains.size() == 1);
    CHECK(back.chains[0].inserts[0].sidechain == 3);
    CHECK(back.chains[0].inserts[1].sidechain == 0);
}

TEST_CASE("a source's Encoder hears the pair of the track its sidechain names; never itself, silence without a pair") {
    //  Catches: an Encoder's sidechain on its own pair, on another source's, or on a pair when its track has none;
    //  or a source that names itself hearing itself.
    const Shape shape{{1}, {}, {}, {}, {{2, 3}, {3, 3}, {5, 4}}, {}, {}, {}};
    const Field f = layOut(set(), shape);  // bus 1 { 2, 3 }, 5; group 3 holds 4
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {3, 6}, {5, 1}}));
    REQUIRE(fp.encoders == std::vector<TrackId>{2, 3, 5});
    CHECK(fp.plan.slots[0].sidechain == 14);  // 2 hears 3, on pair 6, after Main's two
    CHECK(fp.plan.slots[1].sidechain == -1);  // 3 named itself
    CHECK(fp.plan.slots[2].sidechain == -1);  // 5 hears 4, which has no pair of its own
    CHECK(fp.plan.slots[3].sidechain == -1);  // the decoder
}

TEST_CASE("the Encoders' sidechains the set keeps read back") {
    //  Catches: a source's sidechain written but not read, or its two tracks swapped.
    const Shape shape{{}, {}, {}, {}, {{2, 3}, {5, 2}}, {}, {}, {}};
    CHECK(shapeFrom(toJson(shape)).sidechains == shape.sidechains);
}

TEST_CASE("a bus's wires play at its group's fader too; a source's at its own level alone; a return's at its fader") {
    //  Catches: a split group's fader doing nothing, its mute ignored, a wire of the bus without the fader, a
    //  source's fader applied twice -- Live has applied it -- or a return silenced by the mute the device itself set.
    Tree t = sending();  // group 1 { 2 }, 3; returns 10, 11
    t.nodes[0].fader = -6.0;
    t.nodes[1].fader = -20.0;  // a source: Live has it already
    t.nodes[3].fader = -12.0;  // return A
    t.nodes[3].mute = true;    // as the device mutes it
    Shape shape{{1}, {}, {}, {10}, {}, {}, {}, {}};
    shape.routing = {{1, {{Target::main(), 0.0}, {Target::node(10), -6.0}}}};
    const Field f = layOut(t, shape);
    const FieldPlan fp = planOf(f, arrivalsOf({{2, 0}, {3, 1}}));
    //  the bus to Main and A; 2 to the bus; 3 to Main; A to Main; Main to the Decoder
    auto l = live::levels(fp, f, t);
    REQUIRE(l.size() == 6);
    CHECK(l[0] == doctest::Approx(0.501187));             // the bus, -6 dB
    CHECK(l[1] == doctest::Approx(0.501187 * 0.501187));  // its -6 dB wire to A, after its fader
    CHECK(l[2] == 1.0f);                                  // track 2, a source
    CHECK(l[3] == 1.0f);                                  // track 3
    CHECK(l[4] == doctest::Approx(0.251189));             // return A at -12 dB, muted or not
    t.nodes[0].mute = true;
    l = live::levels(fp, f, t);
    CHECK(l[0] == 0.0f);  // a muted group's bus
    CHECK(l[1] == 0.0f);  // and its wire to A
}

TEST_CASE("the host's inputs are sources at the top, after Live's, heard where they arrive, mono on both sides") {
    //  Catches: an input left out of the field or put under a bus, its width lost on the way to the plan, a mono
    //  input's sidechain read as a pair, an input that does not arrive given a node, inputs not kept by the set.
    Shape shape;
    shape.inputs = {{kFirstInput, "vox", 1}, {kFirstInput + 1, "gone", 2}};
    shape.chains = {{kFirstInput, {{7, Effect::Echo, kFirstInput}}}};
    const Field f = layOut(set(), shape);
    REQUIRE(f.parts.size() >= 2);
    CHECK(f.parts[f.parts.size() - 2].node == kFirstInput);
    CHECK(f.parts.back().node == kFirstInput + 1);
    CHECK(f.parts.back().parent == -1);

    std::vector<Arrival> arrivals = arrivalsOf({{1, 0}, {5, 1}});
    arrivals.push_back({kFirstInput, 20, 1});
    const FieldPlan fp = planOf(f, arrivals);
    const auto node = std::find_if(fp.plan.nodes.begin(), fp.plan.nodes.end(),
                                   [](const FieldNode& n) { return n.track == kFirstInput; });
    REQUIRE(node != fp.plan.nodes.end());
    CHECK((node->input == 20 && node->width == 1));
    CHECK(std::none_of(fp.plan.nodes.begin(), fp.plan.nodes.end(),
                       [](const FieldNode& n) { return n.track == kFirstInput + 1; }));
    REQUIRE(fp.effects.size() == 1);
    const Slot& echo = fp.plan.slots[fp.encoders.size()];
    CHECK((echo.sidechain == 20 && echo.sidechainWidth == 1));

    CHECK(shapeFrom(toJson(shape)).inputs == shape.inputs);
}

TEST_CASE("a set's Decoders each have a slot after the inserts, in order; a set naming none has one") {
    //  Catches: a second Decoder left without a slot, two sharing one, the ids lost between the shape and the plan,
    //  a set without decoders getting none, the decoders not kept by the set.
    Shape shape;
    const Field one = layOut(set(), shape);
    CHECK(one.decoders == std::vector<std::uint64_t>{kFirstDecoder});
    shape.decoders = {1, 7};
    const Field two = layOut(set(), shape);
    const FieldPlan fp = planOf(two, arrivalsOf({{1, 0}, {5, 1}}));
    REQUIRE(fp.plan.decoders.size() == 2);
    CHECK(fp.plan.decoders[0] == static_cast<int>(fp.encoders.size() + fp.effects.size()));
    CHECK(fp.plan.decoders[1] == fp.plan.decoders[0] + 1);
    CHECK(fp.decoders == std::vector<std::uint64_t>{1, 7});
    CHECK(fp.plan.slots.size() == fp.encoders.size() + fp.effects.size() + 2);
    CHECK(shapeFrom(toJson(shape)).decoders == shape.decoders);
}

namespace {

//  The set with both groups split, the return taken, two Decoders: buses 1 and 3, sources 2, 4, 5, return 6.
Shape wired(std::vector<Routing> routing = {}) {
    Shape s{{1, 3}, {}, {}, {6}, {}, {}, {1, 2}, {}};
    s.routing = std::move(routing);
    return s;
}

}  // namespace

TEST_CASE("unwired, each node plays where Live would, at 0 dB, and Main into every Decoder") {
    //  Catches: a child of a split group wired past its bus to Main, a return wired nowhere, a Decoder left out of
    //  Main's wires.
    const Field f = layOut(set(), wired());
    CHECK(f.wiresOf(4) == std::vector<Wire>{{Target::node(3), 0.0}});
    CHECK(f.wiresOf(3) == std::vector<Wire>{{Target::node(1), 0.0}});
    CHECK(f.wiresOf(1) == std::vector<Wire>{{Target::main(), 0.0}});
    CHECK(f.wiresOf(6) == std::vector<Wire>{{Target::main(), 0.0}});
    CHECK(f.mainWires == std::vector<Wire>{{Target::decoder(1), 0.0}, {Target::decoder(2), 0.0}});
    CHECK(f.wiresOf(99).empty());
}

TEST_CASE("a node's own wires replace its default; one the field cannot play is left out of it and kept in the shape") {
    //  Catches: the default added beside the user's wires, a node wired nowhere given its default back, a wire to
    //  a Decoder or a bus that is not there played, or dropped from the shape for good.
    const Shape s = wired({{2, {{Target::decoder(2), -6.0}, {Target::decoder(7), 0.0}}},
                           {5, {}},
                           {4, {{Target::node(5), 0.0}, {Target::node(6), -12.0}, {Target::main(), 0.0}}}});
    const Field f = layOut(set(), s);
    CHECK(f.wiresOf(2) == std::vector<Wire>{{Target::decoder(2), -6.0}});
    CHECK(f.wiresOf(5).empty());  // plays nowhere, as asked
    CHECK(f.wiresOf(4) == std::vector<Wire>{{Target::node(6), -12.0}, {Target::main(), 0.0}});  // 5 is no bus
    CHECK(s.routing[0].wires.size() == 2);
    CHECK(layOut(set(), wired({{2, {{Target::decoder(2), -80.0}, {Target::main(), 20.0}}}})).wiresOf(2) ==
          std::vector<Wire>{{Target::decoder(2), kSilentDb}, {Target::main(), kLoudestDb}});
}

TEST_CASE(
    "what may be wired where: a part anywhere but round a loop, a return to Main or a Decoder, Main to Decoders") {
    //  Catches: a bus wired into a bus inside it, which would loop; a part into itself; a return into a return or a
    //  bus; Main into anything but a Decoder; a Decoder the field does not have.
    const Field f = layOut(set(), wired());
    CHECK(canWire(f, 2, Target::node(3)));  // into a bus it is not in: fine
    CHECK(canWire(f, 2, Target::node(6)));
    CHECK(canWire(f, 2, Target::decoder(2)));
    CHECK_FALSE(canWire(f, 1, Target::node(3)));  // 3 plays into 1
    CHECK_FALSE(canWire(f, 3, Target::node(3)));
    CHECK_FALSE(canWire(f, 2, Target::node(5)));  // a source, no bus
    CHECK_FALSE(canWire(f, 2, Target::decoder(9)));
    CHECK(canWire(f, 6, Target::main()));
    CHECK(canWire(f, 6, Target::decoder(1)));
    CHECK_FALSE(canWire(f, 6, Target::node(1)));
    CHECK_FALSE(canWire(f, 6, Target::node(6)));
    CHECK(canWire(f, kMain, Target::decoder(1)));
    CHECK_FALSE(canWire(f, kMain, Target::main()));
    CHECK_FALSE(canWire(f, kMain, Target::node(6)));
    CHECK_FALSE(canWire(f, 42, Target::main()));  // not in the field
}

TEST_CASE("a loop the shape asks for is refused at the wire that would close it") {
    //  Catches: both wires of a loop played, so the plan could not be ordered, or both refused.
    const Field f = layOut(set(), wired({{1, {{Target::node(3), 0.0}}}}));  // 3 already plays into 1
    CHECK(f.wiresOf(3) == std::vector<Wire>{{Target::node(1), 0.0}});
    CHECK(f.wiresOf(1).empty());
}

TEST_CASE("a Decoder's mix is what is wired into it; a node that reaches it another way too is heard twice") {
    //  Catches: a node heard through Main listed as wired in, the twice missed when the other way is through a bus
    //  or a return, or claimed for a node wired only once.
    const Field f = layOut(set(), wired({{2, {{Target::decoder(2), -6.0}}},
                                         {4, {{Target::node(3), 0.0}, {Target::decoder(1), 0.0}}},
                                         {5, {{Target::node(6), 0.0}, {Target::decoder(1), -3.0}}},
                                         {kMain, {{Target::decoder(1), 0.0}}}}));
    CHECK(heardBy(f, 2) == std::vector<Heard>{{2, -6.0, false}});  // Main no longer reaches it
    CHECK(heardBy(f, 1) == std::vector<Heard>{{4, 0.0, true}, {5, -3.0, true}, {kMain, 0.0, false}});
}

TEST_CASE("a wire set on a node with none of its own keeps the ones it played; removed, the rest stay") {
    //  Catches: setting one wire dropping the default beside it, a second wire to the same place added rather than
    //  changed, a level past the range kept, a removal that empties the node.
    Shape s = wired();
    setWire(s, layOut(set(), s), 5, {Target::decoder(2), -6.0});
    CHECK(layOut(set(), s).wiresOf(5) == std::vector<Wire>{{Target::main(), 0.0}, {Target::decoder(2), -6.0}});
    setWire(s, layOut(set(), s), 5, {Target::decoder(2), 99.0});
    CHECK(layOut(set(), s).wiresOf(5).back() == Wire{Target::decoder(2), kLoudestDb});
    CHECK_FALSE(setWire(s, layOut(set(), s), 1, {Target::node(3), 0.0}));  // a loop: refused, the shape as it was
    removeWire(s, layOut(set(), s), 5, Target::main());
    CHECK(layOut(set(), s).wiresOf(5) == std::vector<Wire>{{Target::decoder(2), kLoudestDb}});
    removeWire(s, layOut(set(), s), kMain, Target::decoder(1));
    CHECK(layOut(set(), s).mainWires == std::vector<Wire>{{Target::decoder(2), 0.0}});
}

TEST_CASE("the wires the set keeps read back, a node wired nowhere among them; nonsense is left out") {
    //  Catches: a node wired nowhere read back as unwired, so it plays again; a Decoder's id read as a node's; a
    //  wire without a target taken.
    const Shape s = wired(
        {{2, {{Target::decoder(2), -6.0}, {Target::node(6), -12.5}}}, {5, {}}, {kMain, {{Target::decoder(1), 0.0}}}});
    CHECK(shapeFrom(Json::parse(toJson(s).dump(0))) == s);
    const Shape read = shapeFrom(Json::parse(
        R"({"routing": [{"node": 2, "wires": [{"to": "decoder"}, {"to": "bus", "id": 3}, {"to": "main", "db": -3}]}]})"));
    REQUIRE(read.routing.size() == 1);
    CHECK(read.routing[0].wires == std::vector<Wire>{{Target::main(), -3.0}});
}

TEST_CASE("a level is a gain: silence at the bottom, unity at 0 dB, held at the top") {
    //  Catches: dB read as a gain's ratio of 10 rather than 20, the bottom not silent.
    CHECK(gainOf(kSilentDb) == 0.0f);
    CHECK(gainOf(0.0) == 1.0f);
    CHECK(std::abs(gainOf(-6.0) - 0.501187f) < 1e-5f);
    CHECK(gainOf(40.0) == gainOf(kLoudestDb));
}
