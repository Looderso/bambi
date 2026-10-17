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
    CHECK(row.deviceOf(5) == 2);                                        // the interface, after the microphone
    CHECK(row.deviceEnds() == std::vector<int>{4});                     // the microphone takes no output channel
    CHECK(rowOf(joined, true).deviceEnds() == std::vector<int>{2, 3});  // the box's 2 inputs, the microphone's 1

    std::vector<AudioDevice> wide = system();
    wide.push_back({"big", "Big", 48, 0, 100, 0, false});
    const auto capped = aggregateOf({"i16", "big", "box"}, wide);  // 16 + 48 > 61: the big one and after are left out
    CHECK(capped.size() == 1);
}

TEST_CASE("the settings read back as written, and an older file's missing fields take their defaults") {
    //  Catches: a field written under one key and read under another, a copy's signal lost.
    AudioSettings s;
    s.devices = {"i16", "box"};
    s.buffer = 128;
    s.main = {"i16", 1};
    s.headphones = {"box", 1};
    s.speakersOn = true;
    s.speakers = {"i16", 3};
    s.layout = "4+7+0";
    s.levelDb = -3.0;
    s.copies = {
        {Signal::Headphones, {"box", 3}, false}, {Signal::Headphones, {}, true}, {Signal::Main, {"i16", 15}, false}};
    CHECK(audioSettingsFrom(Json::parse(toJson(s).dump(0))) == s);
    const AudioSettings old = audioSettingsFrom(Json::parse(R"({"devices": ["i16"]})"));
    CHECK(old.buffer == 64);
    CHECK(old.headphonesOn);
    CHECK(old.copies.empty());
}

TEST_CASE("what the host asks of the Decoder: its outputs where kept, the host's channels and the devices") {
    //  Catches: Main's pair or a copy's not kept, a speaker copy kept as a pair, an output on a device that has
    //  gone left playing on whatever is there now.
    const Row row = rowOf(aggregateOf({"i16", "box"}, system()));
    AudioSettings s;
    s.main = {"i16", 1};
    s.headphones = {"box", 1};
    s.speakersOn = true;
    s.speakers = {"i16", 3};
    s.layout = "0+5+0";
    s.copies = {{Signal::Speakers, {"i16", 10}, false}, {Signal::Headphones, {}, true}};
    const DecoderReport d = decoderWith(true, 17, true, 3, {3, 4, 5, 7, 8});  // 5.1, its LFE a gap
    CHECK(keptChannels(s, row, d) == std::vector<int>{1, 2, 10, 11, 12, 14, 15});
    DecoderRequest r = requestFor(s, row, d);
    CHECK(r.headphoneChannel == 17);
    CHECK(r.speakerFirstChannel == 3);
    CHECK(r.layout == "0+5+0");
    CHECK(r.deviceEnds == std::vector<int>{16});
    CHECK((*r.headphonesOn && *r.speakersOn));

    const Row without = rowOf(aggregateOf({"i16"}, system()));
    r = requestFor(s, without, d);
    CHECK_FALSE(*r.headphonesOn);
    CHECK_FALSE(r.headphoneChannel.has_value());
}

TEST_CASE("the routes: every device heard with the slowest, Main later by the field, copies, clashes marked") {
    //  Catches: the quicker device not held back, Main not delayed by the field, a copy at its source's channels,
    //  a copy back to Live on the row, a Main copy sent to Live, an output that is off routed, a clash missed.
    const Row row = rowOf(aggregateOf({"i16", "box"}, system()));
    AudioSettings s;
    s.main = {"i16", 1};
    s.copies = {{Signal::Headphones, {"i16", 15}, false},
                {Signal::Headphones, {}, true},
                {Signal::Main, {}, true},
                {Signal::Main, {"box", 3}, false}};
    const DecoderReport d = decoderWith(true, 17, false, 3, {3, 4});
    const OutputPlan plan = planOutputs(s, row, d, 64);
    CHECK(delayTo(plan, OutputRoute::From::Decoder, 17) == 40);  // the box, 80 against the interface's 120
    CHECK(delayTo(plan, OutputRoute::From::Main, 1) == 64);
    CHECK(delayTo(plan, OutputRoute::From::Main, 19) == 64 + 40);
    CHECK(delayTo(plan, OutputRoute::From::Decoder, 15) == 0);
    CHECK(delayTo(plan, OutputRoute::From::Decoder, 3) == -1);  // the speakers are off
    int toLive = 0;
    for (const OutputRoute& r : plan.routes)
        if (r.to == OutputRoute::To::Loop) {
            ++toLive;
            CHECK(r.from == OutputRoute::From::Decoder);
            CHECK(r.channel == 16 + r.dest);
        }
    CHECK(toLive == 2);
    CHECK(plan.clashes.empty());

    AudioSettings over = s;
    over.main = {"box", 1};  // onto the headphones' pair
    CHECK(planOutputs(over, row, d, 64).clashes == std::vector<int>{17, 18});
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
