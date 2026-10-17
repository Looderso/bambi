// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/outputs.hpp"
#include "bambi/decode/params.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
DecoderState withLayout(const char* name, bool speakers, bool headphones, int hpChannel = 1) {
    DecoderState d;
    d.layout = bs2051Layout(name)->speakers;
    d.speakersPlay = speakers;
    d.headphonesPlay = headphones;
    d.headphoneChannel = hpChannel;
    return d;
}
}  // namespace

TEST_CASE("an output's channels: the pair where it was put, the layout moved as a block from its first channel") {
    //  Catches: the shift dropped, or applied to the headphones; a matrix's outputs not counted from the first.
    DecoderState d = withLayout("0+2+0", true, true, 5);
    CHECK(outputChannels(d, Output::Headphones) == std::vector<int>{5, 6});
    CHECK(outputChannels(d, Output::Speakers) == std::vector<int>{1, 2});
    d.speakerFirstChannel = 3;
    CHECK(outputChannels(d, Output::Speakers) == std::vector<int>{3, 4});
    CHECK(outputChannels(d, Output::Headphones) == std::vector<int>{5, 6});
    d.matrixFile = "iko.sofa";
    d.matrixOutputs = 3;
    CHECK(outputChannels(d, Output::Speakers) == std::vector<int>{3, 4, 5});
    d.layout.clear();
    d.matrixFile.clear();
    CHECK(outputChannels(d, Output::Speakers).empty());
}

TEST_CASE(
    "lighting an output on the other's channels moves it past them while the bus has room, else stops the other") {
    //  Catches: the odd-pair snap dropped (headphones land on 12-13), the room checked against the wrong width,
    //  a collision reported where the other does not play, a layout past the bus reported as fitting.
    const DecoderState d = withLayout("4+7+0", true, false, 1);  // the layout on 1-12, headphones lit on 1-2
    Placement p = placeOutput(d, Output::Headphones, 18);
    CHECK(p.fits);
    CHECK(p.collide);
    CHECK(p.moves);
    CHECK_FALSE(p.stops);
    CHECK(p.movedTo == 13);
    CHECK(p.span == 2);
    p = placeOutput(d, Output::Headphones, 12);
    CHECK(p.stops);
    CHECK_FALSE(p.moves);
    DecoderState quiet = d;
    quiet.speakersPlay = false;
    p = placeOutput(quiet, Output::Headphones, 18);
    CHECK((p.collide && !p.otherOn && !p.moves && !p.stops));
    //  the layout lit over headphones on 1-2 starts after them
    const DecoderState hpFirst = withLayout("4+7+0", false, true, 1);
    p = placeOutput(hpFirst, Output::Speakers, 18);
    CHECK((p.moves && p.movedTo == 3 && p.span == 12));
    p = placeOutput(hpFirst, Output::Speakers, 13);
    CHECK((p.stops && !p.moves));
    //  an eleven-channel layout: the pair after it is 13-14, not 12-13
    DecoderState eleven = d;
    eleven.layout.resize(11);
    p = placeOutput(eleven, Output::Headphones, 18);
    CHECK(p.movedTo == 13);
    //  a layout the bus does not reach does not fit, whatever else is true
    p = placeOutput(d, Output::Speakers, 8);
    CHECK_FALSE(p.fits);
}

TEST_CASE("an output takes no channel the host keeps and plays on one device, moving where there is room") {
    //  Catches: a kept channel taken, an output spanning two devices, the search starting at channel 1 when room
    //  is nearer, headphones moved onto an even pair, a blocked output reported as stopping the other.
    OutputBus bus(26);
    bus.kept = {1, 2};      // Live's Main
    bus.deviceEnds = {16};  // an interface of 16, then one of 10
    const DecoderState hp = withLayout("0+2+0", false, true, 1);
    Placement p = placeOutput(hp, Output::Headphones, bus);
    CHECK((p.blocked && p.moves && !p.stops && p.movedTo == 3));

    DecoderState hpOn15 = withLayout("0+2+0", false, true, 15);
    bus.kept = {};
    p = placeOutput(hpOn15, Output::Headphones, bus);
    CHECK_FALSE(p.blocked);  // 15-16 is the first device's last pair
    hpOn15.headphoneChannel = 16;
    p = placeOutput(hpOn15, Output::Headphones, bus);
    CHECK((p.blocked && p.moves && p.movedTo == 17));  // 16-17 straddles; the next odd pair is the second device's

    //  a layout of 12 from channel 9 crosses into the second device; it moves back to the first, which has room
    DecoderState sp = withLayout("4+7+0", true, false, 1);
    sp.speakerFirstChannel = 9;
    p = placeOutput(sp, Output::Speakers, bus);
    CHECK((p.blocked && p.moves && p.movedTo == 1));
}

TEST_CASE("an output with no room clear of the kept channels on one device does not light") {
    //  Catches: lighting it anyway, or stopping the other in its place.
    OutputBus bus(16);
    bus.deviceEnds = {8};  // two devices of 8: a twelve-channel layout fits on neither
    const DecoderState sp = withLayout("4+7+0", false, true, 15);
    const Placement p = placeOutput(sp, Output::Speakers, bus);
    CHECK((p.fits && p.blocked && p.noRoom && !p.moves && !p.stops));
    const Lit lit = lightOutput(sp, Output::Speakers, true, bus);
    CHECK_FALSE(lit.state.speakersPlay);
    CHECK(lit.state.headphonesPlay);
    CHECK(lit.notice == "no room for the speakers clear of the host's channels, on one device");
}
