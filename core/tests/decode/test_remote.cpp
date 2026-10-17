// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/remote.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
DecoderState stereoOnSpeakers() {
    DecoderState d;
    d.layout = bs2051Layout("0+2+0")->speakers;
    d.layoutName = "0+2+0";
    d.headphonesPlay = true;
    d.speakersPlay = false;
    return d;
}
}  // namespace

TEST_CASE("a report and a request read back as written, and a request names only what it asks") {
    //  Catches: a field written under one key and read under another, a request that writes what it leaves alone
    //  -- it would set the Decoder's other output back to what the host last saw.
    DecoderState d = stereoOnSpeakers();
    d.speakerFirstChannel = 3;
    const DecoderReport report = reportOf(d, 26, -6.0,
                                          "headphones moved to 3\xe2\x80\x93"
                                          "4");
    CHECK(report.layout == "0+2+0");
    CHECK(report.speakerChannels == std::vector<int>{3, 4});
    CHECK(reportFrom(Json::parse(toJson(report).dump(0))) == report);

    DecoderRequest request;
    request.speakersOn = true;
    request.layout = "9+10+3";
    CHECK(requestFrom(Json::parse(toJson(request).dump(0))) == request);
    CHECK(toJson(request).size() == 2);
    CHECK(requestFrom(Json::parse(R"({"speakersOn": "yes", "headphoneChannel": 3, "later": 1})")).headphoneChannel ==
          3);
}

TEST_CASE("a layout the page does not offer by name reports none, but its name and channels") {
    //  Catches: a typed or file layout reported as the built-in it was loaded from -- the sheet would offer it as
    //  chosen, and a request for it would throw the edits away.
    DecoderState d = stereoOnSpeakers();
    d.layoutTyped = true;
    const DecoderReport r = reportOf(d, 8, 0.0, {});
    CHECK(r.layout.empty());
    CHECK(r.layoutName == "0+2+0");
    CHECK(r.speakerChannels == std::vector<int>{1, 2});
}

TEST_CASE("a request is granted by the page's rules: a layout by name, channels the bus holds, outputs lit") {
    //  Catches: lighting that skips the page's rule (the speakers would play over the headphones), a layout set
    //  after the channels are checked, an even or out-of-bus headphone pair taken, a refusal said nowhere.
    DecoderRequest r;
    r.layout = "9+10+3";
    r.speakersOn = true;
    Lit lit = grant(stereoOnSpeakers(), r, 26);
    CHECK(lit.state.layoutName == "9+10+3");
    CHECK(lit.state.speakersPlay);
    CHECK(lit.state.headphonesPlay);
    CHECK(lit.state.speakerFirstChannel == 3);  // moved past the headphones' 1-2
    CHECK(lit.notice ==
          "speakers moved to 3\xe2\x80\x93"
          "26");

    DecoderRequest bad;
    bad.headphoneChannel = 4;
    bad.layout = "7.1.4";
    lit = grant(lit.state, bad, 26);
    CHECK(lit.state.headphoneChannel == 1);
    CHECK(lit.state.layoutName == "9+10+3");
    CHECK(lit.notice == "no built-in layout \"7.1.4\"; headphones cannot start on channel 4");

    DecoderRequest past;
    past.speakerFirstChannel = 4;  // 9+10+3 from 4 needs 27
    past.headphoneChannel = 25;
    lit = grant(lit.state, past, 26);
    CHECK(lit.state.speakerFirstChannel == 3);
    CHECK(lit.state.headphoneChannel == 25);
    CHECK(lit.notice == "speakers cannot start on channel 4");
}

TEST_CASE("lighting an output the bus cannot hold leaves it dark and says what it needs") {
    //  Catches: an output lit past the bus, the needed count or the bus's swapped in the notice.
    DecoderState d = stereoOnSpeakers();
    d.layout = bs2051Layout("9+10+3")->speakers;
    d.layoutName = "9+10+3";
    const Lit lit = lightOutput(d, Output::Speakers, true, 16);
    CHECK_FALSE(lit.state.speakersPlay);
    CHECK(lit.notice == "24 channels needed; the bus has 16");
}

TEST_CASE("a request keeps clear of the host's channels: refused there, and what plays on them moves or stops") {
    //  Catches: a requested channel taken on a kept channel or across two devices, an output left playing on
    //  channels the host has since kept, the bus's shape not read back from a request.
    DecoderRequest shape;
    shape.kept = std::vector<int>{1, 2};
    shape.deviceEnds = std::vector<int>{16};
    CHECK(requestFrom(Json::parse(toJson(shape).dump(0))) == shape);

    OutputBus bus(26);
    bus.kept = {1, 2};
    bus.deviceEnds = {16};
    Lit lit = grant(stereoOnSpeakers(), DecoderRequest{}, bus);  // headphones on 1-2, now kept
    CHECK(lit.state.headphonesPlay);
    CHECK(lit.state.headphoneChannel == 3);
    CHECK(lit.notice ==
          "headphones moved to 3\xe2\x80\x93"
          "4");

    DecoderRequest r;
    r.headphoneChannel = 1;
    r.speakerFirstChannel = 16;  // 16-17 crosses into the second device
    lit = grant(lit.state, r, bus);
    CHECK(lit.state.headphoneChannel == 3);
    CHECK(lit.state.speakerFirstChannel == 1);
    CHECK(lit.notice == "headphones cannot start on channel 1; speakers cannot start on channel 16");

    OutputBus full(2);
    full.kept = {1, 2};
    lit = grant(stereoOnSpeakers(), DecoderRequest{}, full);
    CHECK_FALSE(lit.state.headphonesPlay);
    CHECK(lit.notice == "headphones stopped: no room clear of the host's channels, on one device");

    const DecoderReport report = reportOf(stereoOnSpeakers(), bus, 0.0, {});
    CHECK(reportFrom(Json::parse(toJson(report).dump(0))) == report);
    CHECK(report.kept == std::vector<int>{1, 2});
}
