// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The host's side of the conversation with the bambi Live device: what it answers, and what it keeps.

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

int pairOf(const DeviceSession& s, TrackId t) {
    for (const TrackPair& p : s.pairs())
        if (p.track == t) return p.pair;
    return -1;
}

}  // namespace

TEST_CASE("a hello is welcomed in the host's protocol, saying whether the loop device is there") {
    //  Catches: a welcome that always says the loop device is there, or answers in the device's protocol.
    DeviceSession s({7, false, 0.0, 48000.0});
    const Welcome w = only<Welcome>(s.answer(Hello{2, "12.4", 0.0}));
    CHECK(w.protocol == kProtocol);
    CHECK_FALSE(w.loop);
    CHECK(s.connected());
}

TEST_CASE("a pair for every node at the top that plays to Main with audio, and for nothing else") {
    //  Catches: a pair for a return, for a track inside a group, for one playing elsewhere or with no audio.
    DeviceSession s;
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
    DeviceSession s;
    s.answer(Tree{{node(1), node(2), node(3)}});
    s.answer(Tree{{node(3), node(4), node(1)}});
    CHECK(pairOf(s, 1) == 0);
    CHECK(pairOf(s, 3) == 2);
    CHECK(pairOf(s, 4) == 1);  // the lowest pair nobody holds: 2's
}

TEST_CASE("what one session keeps with the set, another takes back") {
    //  Catches: a save the restore does not read, or a restore that loses a pair -- a reopened set's tracks would
    //  come back on other pairs.
    DeviceSession first;
    first.answer(Tree{{node(7), node(8), node(9)}});
    first.answer(Tree{{node(9), node(8)}});  // 7 gone: its pair is free, the others keep theirs
    const auto saved = first.answer(Tree{{node(9), node(8)}});
    const Save save = only<Save>(saved, 1);

    DeviceSession second;
    second.answer(Restore{save.state});
    second.answer(Tree{{node(8), node(9)}});
    CHECK(pairOf(second, 8) == pairOf(first, 8));
    CHECK(pairOf(second, 9) == pairOf(first, 9));
}

TEST_CASE("the mix's latency and rate go out as the host last set them") {
    //  Catches: an info sent with a latency from before the plan changed.
    DeviceSession s;
    s.setLatency(12.5, 44100.0);
    const Info i = only<Info>(s.answer(Tree{{node(1)}}), 2);
    CHECK(i.latency == 12.5);
    CHECK(i.rate == 44100.0);
}

TEST_CASE("the loop device's channels are named for the pairs, the return on the pair past them") {
    //  Catches: a name one channel off, or the return on a track's pair.
    DeviceSession s({3, true, 0.0, 48000.0});
    s.answer(Tree{{node(1), node(2)}});
    const auto names = s.channelNames();
    REQUIRE(names.size() == 8);
    CHECK(names[0] == "track 1 L");
    CHECK(names[1] == "track 1 R");
    CHECK(names[2] == "track 2 L");
    CHECK(names[3] == "track 2 R");
    CHECK(names[4].empty());
    CHECK(names[6] == "bambi return L");
    CHECK(names[7] == "bambi return R");
}

TEST_CASE("a value the device observed is taken into the tree, and a rename into the channel names") {
    //  Catches: values dropped, so a renamed track keeps its old name on the loop device.
    DeviceSession s({3, true, 0.0, 48000.0});
    s.answer(Tree{{node(1)}});
    s.answer(Value{1, "track 1", "name", Json("kick")});
    s.answer(Value{1, "kick", "send:1", Json(-12.0)});
    s.answer(Value{1, "kick", "mute", Json(1.0)});
    REQUIRE(s.tree().nodes.size() == 1);
    CHECK(s.tree().nodes[0].name == "kick");
    REQUIRE(s.tree().nodes[0].sends.size() == 2);
    CHECK(s.tree().nodes[0].sends[1] == -12.0);
    CHECK(s.tree().nodes[0].mute);
    CHECK(s.channelNames()[0] == "kick L");
}

TEST_CASE("the button, open host and the transport are remembered; a device gone forgets them, not the pairs") {
    //  Catches: a session that stays on after its device went, or one that loses the pairs it must come back to.
    DeviceSession s;
    s.answer(Hello{});
    s.answer(Switched{true});
    s.answer(Show{});
    s.answer(TransportMessage{true, 4.0, 120.0});
    s.answer(Tree{{node(1)}});
    CHECK(s.on());
    CHECK(s.shows() == 1);
    REQUIRE(s.transport().has_value());
    CHECK(s.transport()->ppq == 4.0);
    s.disconnected();
    CHECK_FALSE(s.on());
    CHECK_FALSE(s.connected());
    CHECK_FALSE(s.transport().has_value());
    CHECK(pairOf(s, 1) == 0);
}
