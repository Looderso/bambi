// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The host's side of the conversation with the bambi Live device: what it answers, and what it keeps.

#include <algorithm>
#include <variant>
#include <vector>

#include "bambi/live/session.hpp"
#include "doctest.h"

using namespace bambi;
using namespace bambi::live;

namespace {

Node node(TrackId id, Node::Kind kind = Node::Kind::Track, std::optional<TrackId> parent = std::nullopt) {
    Node n;
    n.id = id;
    n.kind = kind;
    n.parent = parent;
    n.name = "track " + std::to_string(id);
    return n;
}

template <typename T>
const T& only(const std::vector<Message>& answers, std::size_t at = 0) {
    REQUIRE(answers.size() > at);
    REQUIRE(std::holds_alternative<T>(answers[at]));
    return std::get<T>(answers[at]);
}

//  Every id these tests give a node, taken into the field: a session where all of the set plays in bambi.
std::vector<TrackId> everything() {
    std::vector<TrackId> all;
    for (TrackId id = 1; id < 64; ++id) all.push_back(id);
    return all;
}

DeviceSession taking(SessionSettings settings = {}) {
    DeviceSession s(settings);
    Shape all;
    all.taken = everything();
    s.reshape(all);  // before a tree: kept for the first
    return s;
}

Shape all(Shape s) {
    s.taken = everything();
    return s;
}

int pairOf(const DeviceSession& s, TrackId t) {
    for (const TrackPair& p : s.pairs())
        if (p.track == t) return p.pair;
    return -1;
}

}  // namespace

TEST_CASE("a hello is welcomed in the host's protocol, saying whether the loop device is there") {
    //  Catches: a welcome that always says the loop device is there, or answers in the device's protocol.
    DeviceSession s = taking({7, false, 0.0, 48000.0});
    const Welcome w = only<Welcome>(s.answer(Hello{kProtocol, "12.4", 0.0}));
    CHECK(w.protocol == kProtocol);
    CHECK_FALSE(w.loop);
    CHECK(s.connected());
}

TEST_CASE("a device in another protocol is welcomed once, in the host's, and then not heard") {
    /*  Catches: a device from an older build acted on as if it spoke this one -- a set saved with an old device keeps its
        scripts -- or one refused so that it is told nothing, or one never heard again after it has been replaced. */
    DeviceSession s = taking();
    const Welcome w = only<Welcome>(s.answer(Hello{kProtocol - 1, "12.4", 0.0}));
    CHECK(w.protocol == kProtocol);
    CHECK_FALSE(s.compatible());
    Tree t;
    t.nodes.push_back(node(1, Node::Kind::Track));
    CHECK(s.answer(t).empty());  // no pairs, no save: nothing taken from a tree it cannot read
    CHECK(s.pairs().empty());

    //  the device is replaced: it hellos again, in this protocol, and is heard
    s.disconnected();
    CHECK(s.compatible());
    (void)s.answer(Hello{kProtocol, "12.4", 0.0});
    CHECK(s.compatible());
    CHECK_FALSE(s.answer(t).empty());
    CHECK(s.pairs().size() == 1);
}

TEST_CASE("a pair for every node at the top that plays to Main with audio, and for nothing else") {
    //  Catches: a pair for a return, for a track inside a group, for one playing elsewhere or with no audio.
    DeviceSession s = taking();
    Tree t;
    t.nodes.push_back(node(1, Node::Kind::Group));
    t.nodes.push_back(node(2, Node::Kind::Track, TrackId{1}));  // inside the group: the group carries it
    t.nodes.push_back(node(3));
    Node elsewhere = node(4);
    elsewhere.path = Node::Path::Elsewhere;
    t.nodes.push_back(elsewhere);
    Node silent = node(5);
    silent.audio = false;
    t.nodes.push_back(silent);
    t.nodes.push_back(node(6, Node::Kind::Return));
    const auto answers = s.answer(t);
    const Route r = only<Route>(answers);
    REQUIRE(r.tracks.size() == 2);
    CHECK(pairOf(s, 1) == 0);
    CHECK(pairOf(s, 3) == 1);
    CHECK(pairOf(s, 2) == -1);
    CHECK(pairOf(s, 4) == -1);
    CHECK(pairOf(s, 5) == -1);
    CHECK(pairOf(s, 6) == -1);
    (void)only<Save>(answers, 1);
    (void)only<Info>(answers, 2);
}

TEST_CASE("a new tree keeps every track on its pair") {
    //  Catches: pairs assigned from scratch on every tree, which moves every running source when one is added.
    DeviceSession s = taking();
    s.answer(Tree{{node(1), node(2), node(3)}});
    s.answer(Tree{{node(3), node(4), node(1)}});
    CHECK(pairOf(s, 1) == 0);
    CHECK(pairOf(s, 3) == 2);
    CHECK(pairOf(s, 4) == 1);  // the lowest pair nobody holds: 2's
}

TEST_CASE("what one session keeps with the set, another takes back") {
    //  Catches: a save the restore does not read, or a restore that loses a pair -- a reopened set's tracks would
    //  come back on other pairs.
    DeviceSession first = taking();
    first.answer(Tree{{node(7), node(8), node(9)}});
    first.answer(Tree{{node(9), node(8)}});  // 7 gone: its pair is free, the others keep theirs
    const auto saved = first.answer(Tree{{node(9), node(8)}});
    const Save save = only<Save>(saved, 1);

    DeviceSession second = taking();
    second.answer(Restore{save.state});
    second.answer(Tree{{node(8), node(9)}});
    CHECK(pairOf(second, 8) == pairOf(first, 8));
    CHECK(pairOf(second, 9) == pairOf(first, 9));
}

TEST_CASE("the mix's latency and rate go out as the host last set them") {
    //  Catches: an info sent with a latency from before the plan changed.
    DeviceSession s = taking();
    s.setLatency(12.5, 44100.0);
    const Info i = only<Info>(s.answer(Tree{{node(1)}}), 2);
    CHECK(i.latency == 12.5);
    CHECK(i.rate == 44100.0);
}

TEST_CASE("the loop device's channels are named for Main first, then the pairs") {
    //  Catches: a name one channel off, a track named over Main's channels, Main named after the tracks.
    DeviceSession s = taking({3, true, 0.0, 48000.0});
    s.answer(Tree{{node(1), node(2)}});
    const auto names = s.channelNames();
    REQUIRE(names.size() == 8);
    CHECK(names[0] == "main L");
    CHECK(names[1] == "main R");
    CHECK(names[2] == "track 1 L");
    CHECK(names[3] == "track 1 R");
    CHECK(names[4] == "track 2 L");
    CHECK(names[5] == "track 2 R");
    CHECK(names[6].empty());
}

TEST_CASE("a value the device observed is taken into the tree, and a rename into the channel names") {
    //  Catches: values dropped, so a renamed track keeps its old name on the loop device.
    DeviceSession s = taking({3, true, 0.0, 48000.0});
    s.answer(Tree{{node(1)}});
    s.answer(Value{1, "track 1", "name", Json("kick")});
    s.answer(Value{1, "kick", "send:1", Json(-12.0)});
    s.answer(Value{1, "kick", "mute", Json(1.0)});
    REQUIRE(s.tree().nodes.size() == 1);
    CHECK(s.tree().nodes[0].name == "kick");
    REQUIRE(s.tree().nodes[0].sends.size() == 2);
    CHECK(s.tree().nodes[0].sends[1] == -12.0);
    CHECK(s.tree().nodes[0].mute);
    CHECK(s.channelNames()[2] == "kick L");
}

TEST_CASE("the button, open host and the transport are remembered; a device gone forgets them, not the pairs") {
    //  Catches: a session that stays on after its device went, or one that loses the pairs it must come back to.
    DeviceSession s = taking();
    s.answer(Hello{});
    s.answer(Switched{true});
    s.answer(Show{});
    s.answer(TransportMessage{true, 4.0, 120.0});
    s.answer(Tree{{node(1)}});
    CHECK(s.on());
    CHECK(s.shows() == 1);
    REQUIRE(s.transport().has_value());
    CHECK(s.transport().value().ppq == 4.0);
    s.disconnected();
    CHECK_FALSE(s.on());
    CHECK_FALSE(s.connected());
    CHECK_FALSE(s.transport().has_value());
    CHECK(pairOf(s, 1) == 0);
}

TEST_CASE("what the host keeps beside the pairs goes into every save and comes back with a restore") {
    //  Catches: a part kept but never saved, a restore that drops it, or one that keeps a part from before it --
    //  a reopened set's plugins would come back as new, or as another set's.
    DeviceSession first = taking();
    first.keep("plugins", Json("encoder states"));
    first.answer(Tree{{node(1)}});
    const Save save = first.save();
    REQUIRE(save.state.find("plugins") != nullptr);

    DeviceSession second = taking();
    second.keep("plugins", Json("older"));
    second.keep("other", Json(1.0));
    second.answer(Restore{save.state});
    REQUIRE(second.kept("plugins") != nullptr);
    CHECK(second.kept("plugins")->stringOr("") == "encoder states");
    CHECK(second.kept("other") == nullptr);
    second.answer(Tree{{node(1)}});
    CHECK(pairOf(second, 1) == 0);
}

TEST_CASE("a part named like the pairs never replaces them") {
    //  Catches: a kept part that overwrites the pairs in the save.
    DeviceSession s = taking();
    s.answer(Tree{{node(4)}});
    s.keep("pairs", Json("not pairs"));
    const Save save = s.save();
    const Json* pairs = save.state.find("pairs");
    REQUIRE(pairs != nullptr);
    CHECK(pairs->isArray());
}

TEST_CASE("the track on a pair, as the tree has it now") {
    //  Catches: the node of another pair, a node for an empty pair, or a name from before a rename.
    DeviceSession s = taking({3, true, 0.0, 48000.0});
    s.answer(Tree{{node(1), node(2)}});
    s.answer(Value{2, "track 2", "name", Json("bass")});
    REQUIRE(s.trackOn(1) != nullptr);
    CHECK(s.trackOn(1)->id == 2);
    CHECK(s.trackOn(1)->name == "bass");
    CHECK(s.trackOn(2) == nullptr);
}

TEST_CASE("a new shape lays the field out again: a split group's children get pairs, the group lets go of its own") {
    //  Catches: a shape that changes nothing until the next tree, or a group that keeps its pair beside its
    //  children's -- it would be heard twice.
    DeviceSession s = taking();
    CHECK(s.reshape(all(Shape{{1}, {}, {}, {}, {}, {}, {}, {}})).empty());  // no tree yet: nothing to route
    s.reshape(all(Shape{}));
    s.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}), node(3)}});
    CHECK(pairOf(s, 1) == 0);
    CHECK(pairOf(s, 3) == 1);
    const auto answers = s.reshape(all(Shape{{1}, {}, {}, {}, {}, {}, {}, {}}));
    REQUIRE(answers.size() == 3);
    (void)only<Route>(answers);
    CHECK(pairOf(s, 1) == -1);
    CHECK(pairOf(s, 2) == 0);  // the pair its group let go of
    CHECK(pairOf(s, 3) == 1);  // kept
    REQUIRE(s.field().parts.size() == 3);
    CHECK(s.field().parts[0].bus);
}

TEST_CASE("the shape goes into every save and comes back with a restore; an older set's split, adopted") {
    //  Catches: a shape the set never keeps, so a reopened set plays every group whole again, or an older set's
    //  split group adopted but saved as it came, to be adopted again.
    DeviceSession first = taking();
    first.reshape(Shape{{1}, {1}, {}, {}, {}, {}, {}, {}});  // as an older set kept it; 3 not taken
    first.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}), node(3)}});
    CHECK(first.shape() == Shape{{}, {2}, {}, {}, {}, {}, {}, {}});
    DeviceSession second = taking();
    second.answer(Restore{first.save().state});
    CHECK(second.shape() == Shape{{}, {2}, {}, {}, {}, {}, {}, {}});
    second.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}), node(3)}});
    CHECK(pairOf(second, 2) == pairOf(first, 2));
    CHECK(pairOf(second, 3) == -1);
}

TEST_CASE("the routing says which of Live's returns the field plays, so the device silences them in Live") {
    //  Catches: a field return left playing in Live, heard twice; or one the field gave back left silent.
    DeviceSession s = taking();
    s.answer(Tree{{node(1), node(7, Node::Kind::Return), node(8, Node::Kind::Return)}});
    const auto taken = s.reshape(all(Shape{{}, {}, {}, {8}, {}, {}, {}, {}}));
    CHECK(only<Route>(taken).silent == std::vector<TrackId>{8});
    const auto given = s.reshape(all(Shape{}));
    CHECK(only<Route>(given).silent.empty());
}

TEST_CASE("tracks bambi plays grouped in Live keep their Encoders: their new groups become their buses") {
    //  Catches: a group formed around sources taken whole -- their Encoders, inserts and positions gone -- or a
    //  group of untouched tracks taken for nothing.
    DeviceSession s;
    s.reshape(Shape{{}, {2, 3, 4}, {}, {}, {}, {}, {}, {}});
    s.answer(Tree{{node(2), node(3), node(4)}});  // three sources at the top
    REQUIRE(pairOf(s, 2) >= 0);
    //  2 and 3 grouped into 1, which is grouped into 9 with a fresh group 5 of a new track 6
    s.answer(Tree{{node(9, Node::Kind::Group), node(1, Node::Kind::Group, TrackId{9}),
                   node(2, Node::Kind::Track, TrackId{1}), node(3, Node::Kind::Track, TrackId{1}),
                   node(5, Node::Kind::Group, TrackId{9}), node(6, Node::Kind::Track, TrackId{5}), node(4)}});
    CHECK(s.shape().taken == std::vector<TrackId>{2, 3, 4});
    CHECK(pairOf(s, 2) >= 0);
    CHECK(pairOf(s, 3) >= 0);
    CHECK(pairOf(s, 5) == -1);  // the fresh group, in Live with its new track
    CHECK(pairOf(s, 6) == -1);
    REQUIRE(s.field().parts.size() == 5);  // bus 9 { bus 1 { 2, 3 } }, 4
    CHECK(s.field().parts[0].bus);
    CHECK(s.field().parts[1].bus);
}

TEST_CASE("a track bambi plays, filed by Live into a group encoded whole, opens the group and keeps its Encoder") {
    //  Catches: the track's mark hidden under the group's, so its Encoder, inserts and pair are gone and it is heard
    //  through the group's Encoder instead -- or the group's other tracks dropped to Live.
    DeviceSession s;
    s.reshape(Shape{{}, {1, 4}, {}, {}, {}, {}, {}, {}});
    s.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}), node(4)}});
    const int pair = pairOf(s, 4);
    REQUIRE(pair >= 0);
    //  Live files 4 into group 1
    s.answer(Tree{
        {node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1}), node(4, Node::Kind::Track, TrackId{1})}});
    CHECK(pairOf(s, 4) == pair);
    CHECK(pairOf(s, 2) >= 0);  // heard in the field before, through the group: taken in its place
    CHECK(pairOf(s, 1) == -1);
}

TEST_CASE("a group the user takes whole stays whole") {
    //  Catches: the user's "encode whole" undone by the very shape it makes, because its tracks were sources.
    DeviceSession s = taking();
    s.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1})}});
    s.reshape(all(Shape{{1}, {}, {}, {}, {}, {}, {}, {}}));
    REQUIRE(pairOf(s, 2) >= 0);
    s.reshape(all(Shape{}));
    CHECK(s.shape().split.empty());
    CHECK(pairOf(s, 1) >= 0);
    CHECK(pairOf(s, 2) == -1);
    s.answer(Tree{{node(1, Node::Kind::Group), node(2, Node::Kind::Track, TrackId{1})}});  // the same set again
    CHECK(s.shape().split.empty());
}

TEST_CASE("before a tree has come, the session says it has none rather than an empty set") {
    //  Catches: a restore read as a set with no tracks -- whoever forgets what the set no longer holds would forget
    //  everything the restore just gave back.
    DeviceSession s = taking();
    s.answer(Restore{Json::object()});
    CHECK_FALSE(s.hasTree());
    s.answer(Tree{{node(1)}});
    CHECK(s.hasTree());
    s.disconnected();
    CHECK(s.hasTree());  // the set is as it was until a new tree says otherwise
}

TEST_CASE("the arrangement the device sends is kept as sent, the last one standing") {
    //  Catches: an arrangement dropped, answered as if it asked something, or a later one not replacing it.
    DeviceSession s = taking();
    Arrangement a;
    a.tracks = {{1, {{0.0, 8.0, "intro", "#ffffff", false}}}};
    CHECK(s.answer(a).empty());
    CHECK(s.arrangement() == a);
    a.locators = {{16.0, "verse"}};
    s.answer(a);
    CHECK(s.arrangement().locators.size() == 1);
}

TEST_CASE("a set opens with everything in Live; a track taken gets a pair, and keeps it grouped in Live, alone") {
    //  Catches: tracks moved before the user takes any, a taken track's pair lost when Live groups it, or the
    //  tracks grouped with it taken along.
    DeviceSession s;
    s.answer(Tree{{node(2), node(3), node(4)}});
    CHECK(s.pairs().empty());
    Shape one;
    one.taken = {2};
    s.reshape(one);
    const int pair = pairOf(s, 2);
    CHECK(pair >= 0);
    CHECK(pairOf(s, 3) == -1);
    s.answer(Tree{{node(9, Node::Kind::Group), node(1, Node::Kind::Group, TrackId{9}),
                   node(2, Node::Kind::Track, TrackId{1}), node(3, Node::Kind::Track, TrackId{1}), node(4)}});
    CHECK(s.shape().taken == std::vector<TrackId>{2});
    CHECK(pairOf(s, 2) == pair);
    CHECK(pairOf(s, 3) == -1);  // grouped with it in Live, and left there
    CHECK(pairOf(s, 4) == -1);
}
