// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/io.hpp"
#include "doctest.h"

using namespace bambi;
using namespace bambi::live;

namespace {
//  An interface of 16 outputs, slow; a headphone box of 4, quicker; a Bluetooth pair; a microphone.
std::vector<AudioDevice> system() {
    return {{"i16", "Interface", 16, 16, 120, 100, false},
            {"box", "Headphone box", 4, 2, 80, 70, false},
            {"bt", "AirPods", 2, 1, 7000, 7000, true},
            {"mic", "Microphone", 0, 1, 0, 90, false}};
}

DecoderReport decoderWith(bool headphones, int hp, bool speakers, int first, std::vector<int> channels) {
    DecoderReport d;
    d.headphonesOn = headphones;
    d.headphoneChannel = hp;
    d.speakersOn = speakers;
    d.speakerFirstChannel = first;
    d.speakerChannels = std::move(channels);
    return d;
}

int delayTo(const OutputPlan& plan, OutputRoute::From from, int dest) {
    for (const OutputRoute& r : plan.routes)
        if (r.from == from && r.to == OutputRoute::To::Row && r.dest == dest) return r.delay;
    return -1;
}
}  // namespace

TEST_CASE("the row: the chosen devices that play, in order, each whole, a channel named by its device") {
    //  Catches: a Bluetooth device or one without outputs on the row, a device chosen twice counted twice, a
    //  device cut in half at the cap, a channel found by its place rather than its device.
    const std::vector<AudioDevice> joined = aggregateOf({"box", "bt", "mic", "i16", "box", "gone"}, system());
    REQUIRE(joined.size() == 3);  // the microphone too: its inputs are on the input row
    CHECK(joined[0].uid == "box");
    const Row row = rowOf(joined);
    CHECK(row.width() == 20);
    CHECK(row.at({"i16", 1}) == 5);
    CHECK(row.at({"i16", 17}) == 0);
    CHECK(row.at({"bt", 1}) == 0);
    CHECK(row.channel(6) == DeviceChannel{"i16", 2});
    CHECK(row.channel(21).none());
    CHECK(row.deviceOf(4) == 0);
    CHECK(row.deviceOf(5) == 2);               // the interface, after the microphone
    CHECK(rowOf(joined, true).width() == 19);  // the box's 2 inputs, the microphone's 1, the interface's 16

    std::vector<AudioDevice> wide = system();
    wide.push_back({"big", "Big", 48, 0, 100, 0, false});
    const auto capped = aggregateOf({"i16", "big", "box"}, wide);  // 16 + 48 > 61: the big one and after are left out
    CHECK(capped.size() == 1);
}

TEST_CASE("the settings read back as written, and an older file's missing fields take their defaults") {
    //  Catches: a field written under one key and read under another, a copy's or a placement's Decoder or output
    //  lost, Main read back as a Decoder's output.
    AudioSettings s;
    s.devices = {"i16", "box"};
    s.buffer = 128;
    s.main = {"i16", 1};
    s.placed = {{{1, Output::Headphones}, {"box", 1}}, {{7, Output::Speakers}, {"i16", 3}}};
    s.copies = {{{1, Output::Headphones}, {"box", 3}, false},
                {{7, Output::Headphones}, {}, true},
                {kLiveMain, {"i16", 15}, false}};
    s.liveMidiMs = 23.5;
    CHECK(audioSettingsFrom(Json::parse(toJson(s).dump(0))) == s);
    CHECK(s.at({7, Output::Speakers}) == DeviceChannel{"i16", 3});
    CHECK(s.at({7, Output::Headphones}).none());
    CHECK(s.at(kLiveMain) == s.main);
    //  a file from when there was one Decoder
    const AudioSettings one = audioSettingsFrom(Json::parse(
        R"({"headphones": {"device": "box", "number": 1},
            "copies": [{"of": "headphones", "toLive": true}, {"of": "main", "at": {"device": "box", "number": 5}}]})"));
    CHECK(one.at({kFirstDecoder, Output::Headphones}) == DeviceChannel{"box", 1});
    REQUIRE(one.copies.size() == 2);
    CHECK(one.copies[0].of == Out{kFirstDecoder, Output::Headphones});
    CHECK(one.copies[1].of.main());
    const AudioSettings old = audioSettingsFrom(Json::parse(R"({"devices": ["i16"]})"));
    CHECK(old.buffer == 64);
    CHECK(old.liveMidiMs == 0.0);
    CHECK(old.placed.empty());
    CHECK(old.copies.empty());
}

TEST_CASE("an output takes its bus channels as a block from where it is placed, within the row") {
    //  Catches: a layout's gap closed up -- its LFE channel would land on a loudspeaker -- the block not moved to its
    //  place, an output that is off taking channels, a channel past the row kept.
    const Row row = rowOf(aggregateOf({"i16", "box"}, system()));
    const std::vector<PlayingDecoder> decoders{{1, decoderWith(true, 1, true, 3, {3, 4, 5, 7, 8}), 64}};
    CHECK(takenBy({1, Output::Speakers}, {"i16", 9}, row, decoders) == std::vector<int>{9, 10, 11, 13, 14});
    CHECK(takenBy({1, Output::Headphones}, {"box", 3}, row, decoders) == std::vector<int>{19, 20});
    CHECK(takenBy({1, Output::Speakers}, {"box", 1}, row, decoders) == std::vector<int>{17, 18, 19});
    CHECK(takenBy({2, Output::Headphones}, {"box", 1}, row, decoders).empty());
    const std::vector<PlayingDecoder> quiet{{1, decoderWith(false, 1, false, 3, {3, 4}), 64}};
    CHECK(takenBy({1, Output::Headphones}, {"box", 1}, row, quiet).empty());
}

TEST_CASE("the routes: two Decoders each where placed, every device with the slowest, Main late, clashes marked") {
    //  Catches: the second Decoder's outputs routed from the first's bus, a quicker Decoder not held back to the
    //  slowest, the quicker device not held back, Main not late by the field, a copy at its source's channels, a
    //  copy back to Live on the row, Main sent back to Live, an output placed nowhere heard, a clash missed.
    const Row row = rowOf(aggregateOf({"i16", "box"}, system()));
    AudioSettings s;
    s.main = {"i16", 1};
    s.placed = {{{1, Output::Headphones}, {"box", 1}}, {{2, Output::Headphones}, {"i16", 3}}};
    s.copies = {
        {{1, Output::Headphones}, {"i16", 15}, false}, {{2, Output::Headphones}, {}, true}, {kLiveMain, {}, true}};
    const std::vector<PlayingDecoder> decoders{{1, decoderWith(true, 1, true, 3, {3, 4}), 64},
                                               {2, decoderWith(true, 5, false, 1, {}), 40}};
    const OutputPlan plan = planOutputs(s, row, decoders, 300);
    const auto route = [&](int dest) {
        for (const OutputRoute& r : plan.routes)
            if (r.to == OutputRoute::To::Row && r.dest == dest) return r;
        return OutputRoute{};
    };
    CHECK((route(17).decoder == 0 && route(17).channel == 1 && route(17).delay == 40));  // the box: 80 to 120
    CHECK((route(3).decoder == 1 && route(3).channel == 5 && route(3).delay == 24));     // 64 against 40
    CHECK((route(15).decoder == 0 && route(15).channel == 1 && route(15).delay == 0));
    CHECK(delayTo(plan, OutputRoute::From::Main, 1) == 300);
    CHECK(route(5).dest == 0);  // decoder 1's speakers play and are placed nowhere: not heard
    int toLive = 0;
    for (const OutputRoute& r : plan.routes)
        if (r.to == OutputRoute::To::Loop) {
            ++toLive;
            CHECK((r.from == OutputRoute::From::Decoder && r.decoder == 1 && r.channel == 4 + r.dest));
        }
    CHECK(toLive == 2);
    CHECK(plan.clashes.empty());

    AudioSettings over = s;
    over.placed[1].at = {"box", 2};  // decoder 2's headphones onto the box's 2-3, over decoder 1's 1-2
    CHECK(planOutputs(over, row, decoders, 300).clashes == std::vector<int>{18});
}

TEST_CASE("an input arrives after the loop device's channels, on the channel this computer keeps for it") {
    //  Catches: the loop device's channels not counted before the inputs, an input on a device not chosen or kept
    //  nowhere arriving anyway, a stereo input across two devices, the input bindings not read back.
    const Row row = rowOf(aggregateOf({"i16", "box", "mic"}, system()), true);  // 16 + 2 + 1 inputs
    CHECK(row.width() == 19);
    AudioSettings s;
    s.inputs = {{kFirstInput, {"mic", 1}},
                {kFirstInput + 1, {"i16", 3}},
                {kFirstInput + 2, {"box", 2}},
                {kFirstInput + 3, {"bt", 1}}};
    const std::vector<HostInput> inputs{{kFirstInput, "vox", 1},
                                        {kFirstInput + 1, "keys", 2},
                                        {kFirstInput + 2, "across", 2},
                                        {kFirstInput + 3, "airpods", 1},
                                        {kFirstInput + 4, "nowhere", 1}};
    const std::vector<Arrival> a = inputArrivals(s, inputs, row, 16);
    REQUIRE(a.size() == 2);
    CHECK(a[0] == Arrival{kFirstInput, 16 + 18, 1});
    CHECK(a[1] == Arrival{kFirstInput + 1, 16 + 2, 2});
    CHECK(audioSettingsFrom(Json::parse(toJson(s).dump(0))).inputs == s.inputs);
}
