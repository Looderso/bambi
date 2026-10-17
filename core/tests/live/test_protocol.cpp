// SPDX-License-Identifier: GPL-3.0-or-later
//
//  What the bambi Live device and the host say to each other. The device's lines below are written as its Max
//  script writes them -- JSON.stringify, its fields in its order -- so a change here that the script does not
//  share fails here first.

#include <string>
#include <variant>

#include "bambi/live/protocol.hpp"
#include "doctest.h"

using namespace bambi;
using namespace bambi::live;

namespace {

template <typename T>
T read(const std::string& line) {
    std::string error;
    const auto m = parse(line, &error);
    REQUIRE_MESSAGE(m.has_value(), error);
    REQUIRE(std::holds_alternative<T>(*m));
    return std::get<T>(*m);
}

template <typename T>
T again(const T& m) {
    return read<T>(serialise(Message{m}));
}

}  // namespace

TEST_CASE("the device's own lines are read as it means them") {
    //  Catches: a field read under another name than the script writes, or Max's 0 and 1 not taken as flags.
    const Hello h = read<Hello>(R"({"type":"hello","protocol":1,"live":"12.4","rate":0})");
    CHECK(h.protocol == 1);
    CHECK(h.live == "12.4");
    CHECK(accepts(h));

    CHECK(read<Switched>(R"({"type":"bambi","on":1})").on);
    CHECK_FALSE(read<Switched>(R"({"type":"bambi","on":0})").on);

    const Tree t =
        read<Tree>(R"({"type":"tree","nodes":[{"id":1,"kind":"group","parent":null,"path":"main","name":"1-Group",)"
                   R"("colour":"#00bfaf","audio":1,"sends":[null,-68],"fader":0,"mute":0,"solo":1,"pan":-0.25},)"
                   R"({"id":2,"kind":"track","parent":1,"path":"main","name":"kick","colour":"#00bfaf","audio":1,)"
                   R"("sends":[],"fader":-0.8,"mute":1,"solo":0,"pan":0},)"
                   R"({"id":6,"kind":"return","parent":null,"path":"elsewhere","name":"B-Delay","colour":"#ffa529",)"
                   R"("audio":1,"sends":[],"fader":null,"mute":0,"solo":0,"pan":0}]})");
    REQUIRE(t.nodes.size() == 3);
    CHECK(t.nodes[0].kind == Node::Kind::Group);
    CHECK_FALSE(t.nodes[0].parent.has_value());
    CHECK(t.nodes[0].sends.size() == 2);
    CHECK_FALSE(t.nodes[0].sends[0].has_value());  // null: silence
    CHECK(t.nodes[0].sends[1] == -68.0);
    CHECK(t.nodes[0].solo);
    CHECK(t.nodes[0].pan == -0.25);
    CHECK(t.nodes[1].parent == TrackId{1});
    CHECK(t.nodes[1].fader == -0.8);
    CHECK(t.nodes[1].mute);
    CHECK(t.nodes[2].kind == Node::Kind::Return);
    CHECK(t.nodes[2].path == Node::Path::Elsewhere);
    CHECK_FALSE(t.nodes[2].fader.has_value());

    const Value v = read<Value>(R"({"type":"value","id":4,"name":"clap","what":"send:0","value":-68})");
    CHECK(v.id == 4);
    CHECK(v.what == "send:0");
    CHECK(v.value.numberOr(0) == -68.0);

    const TransportMessage tr = read<TransportMessage>(R"({"type":"transport","playing":1,"ppq":127.355,"bpm":120})");
    CHECK(tr.playing);
    CHECK(tr.ppq == 127.355);
    CHECK(tr.bpm == 120.0);

    const Routed r =
        read<Routed>(R"({"type":"routed","tracks":[{"id":1,"name":"1-Group","ok":true,"to":"Ext. Out 3/4"},)"
                     R"({"id":9,"ok":false,"error":"no such track"}]})");
    REQUIRE(r.tracks.size() == 2);
    CHECK(r.tracks[0].ok);
    CHECK(r.tracks[0].to == "Ext. Out 3/4");
    CHECK_FALSE(r.tracks[1].ok);
    CHECK(r.tracks[1].error == "no such track");

    const Restore re = read<Restore>(R"({"type":"restore","state":{"pairs":[{"id":1,"pair":0}]}})");
    CHECK(re.state.find("pairs") != nullptr);
    (void)read<Show>(R"({"type":"show"})");
}

TEST_CASE("every message comes back from the wire as it went") {
    //  Catches: a field written and not read, or read from the wrong field -- a pair taken from the track's id.
    Node n;
    n.id = 3;
    n.kind = Node::Kind::Group;
    n.parent = 7;
    n.path = Node::Path::Elsewhere;
    n.name = "Drums";
    n.colour = "#e0904a";
    n.audio = false;
    n.sends = {std::nullopt, -12.5};
    n.fader = -3.0;
    n.mute = true;
    n.solo = true;
    n.pan = 0.5;
    const Tree t{{n}};
    CHECK(again(t) == t);

    const Hello h{1, "12.4", 48000.0};
    CHECK(again(h) == h);
    CHECK(again(Switched{true}) == Switched{true});
    const Routed r{{{2, "kick", true, "Ext. Out 5/6", ""}, {4, "clap", false, "", "refused"}}};
    CHECK(again(r) == r);
    const Welcome w{1, false};
    CHECK(again(w) == w);
    const Route ro{{{5, 0}, {11, 3}}, {20, 21}};
    CHECK(again(ro) == ro);
    const Info i{12.0, 48000.0, {{4, "its sends are post-fader"}, {9, "lost"}}};
    CHECK(again(i) == i);
    CHECK(again(Status{"busy"}) == Status{"busy"});
    CHECK(again(CreateReturn{"bambi · Plate"}) == CreateReturn{"bambi · Plate"});
    (void)again(Busy{});
    (void)again(Show{});

    const TransportMessage tr{true, 33.25, 97.0};
    const TransportMessage back = again(tr);
    CHECK(back.playing == tr.playing);
    CHECK(back.ppq == tr.ppq);
    CHECK(back.bpm == tr.bpm);

    Json state = Json::object();
    state.set("pairs", Json::array());
    CHECK(serialise(Message{again(Save{state})}) == serialise(Message{Save{state}}));
    Value v{4, "clap", "name", Json("clap 2")};
    CHECK(serialise(Message{again(v)}) == serialise(Message{v}));
}

TEST_CASE("a hello in another protocol is read, and refused by accepts") {
    //  Catches: a version check that accepts anything, or one that makes the line unreadable -- the answer to a hello
    //  in the wrong protocol is still a welcome saying which this side speaks.
    const Hello h = read<Hello>(R"({"type":"hello","protocol":2,"live":"13.0","rate":0})");
    CHECK(h.protocol == 2);
    CHECK_FALSE(accepts(h));
}

TEST_CASE("an unknown type, or a line that is no object, is refused; an unknown field is not") {
    //  Catches: guessing a message from a type nobody knows, or refusing a line for a field added later.
    std::string error;
    CHECK_FALSE(parse(R"({"type":"teleport"})", &error).has_value());
    CHECK(error.find("teleport") != std::string::npos);
    CHECK_FALSE(parse("[1,2]", &error).has_value());
    CHECK_FALSE(parse("not json", &error).has_value());
    const Status s = read<Status>(R"({"type":"status","text":"ready","colour":"green"})");
    CHECK(s.text == "ready");
}

TEST_CASE("a field missing takes its default") {
    //  Catches: a missing field read as garbage -- a route without tracks, a node without sends.
    CHECK(read<Route>(R"({"type":"route"})").tracks.empty());
    const Tree t = read<Tree>(R"({"type":"tree","nodes":[{"id":5}]})");
    REQUIRE(t.nodes.size() == 1);
    CHECK(t.nodes[0].kind == Node::Kind::Track);
    CHECK(t.nodes[0].audio);
    CHECK(t.nodes[0].sends.empty());
    CHECK_FALSE(t.nodes[0].fader.has_value());
    CHECK(read<TransportMessage>(R"({"type":"transport"})").bpm == 120.0);
}
