// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bambi/math/sh.hpp"
#include "bambi/reverb/early.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kFs = 48000.0;

Room hall() { return deriveRoom({20.0, RoomShape::Hall, 1.9, 0.5, 0.6}); }

struct Heard {
    std::vector<float> field, scattered;
    int channels;
};

//  An impulse from a direction, encoded on the bus, through the reflections.
Heard impulseFrom(EarlyReflections& early, int order, Vec3 direction, double seconds = 0.5) {
    const int C = numChannels(order), frames = static_cast<int>(seconds * kFs);
    std::vector<float> in(static_cast<std::size_t>(frames * C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(direction, order, y);
    for (int c = 0; c < C; ++c) in[static_cast<std::size_t>(c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    Heard h{std::vector<float>(in.size()), std::vector<float>(static_cast<std::size_t>(frames * kVirtualSources)), C};
    early.reset();
    early.process(in.data(), h.field.data(), h.scattered.data(), frames);
    return h;
}

double omniEnergy(const Heard& h) {
    double e = 0.0;
    for (std::size_t f = 0; f < h.field.size() / static_cast<std::size_t>(h.channels); ++f)
        e += static_cast<double>(h.field[f * h.channels]) * h.field[f * h.channels];
    return e;
}

//  Where the reflections come from on balance: the first-order channels against the omni, summed.
Vec3 leaning(const Heard& h) {
    Vec3 v{0, 0, 0};
    for (std::size_t f = 0; f < h.field.size() / static_cast<std::size_t>(h.channels); ++f) {
        const float* x = h.field.data() + f * h.channels;
        v = v + Vec3{x[3], x[1], x[2]} * static_cast<double>(x[0]);  // ACN 3, 1, 2 are x, y, z
    }
    return v;
}

}  // namespace

TEST_CASE("the bus's reflections are as loud as one exact source's would be") {
    /*  The level is matched, not assumed: copies of an image a few milliseconds apart add
        their energies and copies together their amplitudes. On average over directions, what comes out
        in the omni channel for a unit impulse has the energy of the exact model's mirror images -- less
        what their low-passes take, which is why it is a little under. Catches: the matching left out,
        and the scattered part heard as reflections too. */
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(hall(), 6.0);
    CHECK(early.liveTaps() > 100);
    CHECK(early.level() > 0.2);
    CHECK(early.level() < 5.0);
    double heard = 0.0;
    int n = 0;
    for (const Vec3& u : {fromAzEl(0.3, 0.1), fromAzEl(2.0, -0.4), fromAzEl(-1.2, 0.6), fromAzEl(3.0, 0.0),
                          fromAzEl(-2.4, -0.2), fromAzEl(1.1, 1.0)}) {
        heard += omniEnergy(impulseFrom(early, 3, u));
        ++n;
    }
    heard /= n;
    INFO("heard ", heard, " model ", early.mirrorEnergy(), " level ", early.level(), " taps ", early.liveTaps());
    //  Measured: with copies spread 3 ms apart their energies simply add, and the match is within half a
    //  decibel of 1 in every room from 4 m to 38 m -- 0.95 to 1.00. It would matter at a smaller spread,
    //  where copies land together; it is asserted as what it is.
    CHECK(early.level() > 0.9);
    CHECK(early.level() < 1.1);
    CHECK(heard < 1.15 * early.mirrorEnergy());
    CHECK(heard > 0.55 * early.mirrorEnergy());
}

TEST_CASE("nothing is heard before the first reflection, and silence in is silence out") {
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(hall(), 6.0);
    const Heard h = impulseFrom(early, 3, fromAzEl(0.5, 0.0));
    const int first = static_cast<int>(early.firstDelaySeconds() * kFs);
    REQUIRE(first > 100);  // a hall's first reflection is milliseconds away
    for (int f = 0; f < first; ++f)
        for (int c = 0; c < 16; ++c) REQUIRE(h.field[static_cast<std::size_t>(f * 16 + c)] == 0.0f);
    //  and it IS heard on that sample, not one later: a low-pass delays nothing's onset
    CHECK(h.field[static_cast<std::size_t>(first * 16)] != 0.0f);

    std::vector<float> none(16 * 2000, 0.0f), field(none.size(), 1.0f),
        sc(static_cast<std::size_t>(kVirtualSources * 2000), 1.0f);
    early.reset();
    early.process(none.data(), field.data(), sc.data(), 2000);
    for (const float v : field) REQUIRE(v == 0.0f);
    for (const float v : sc) REQUIRE(v == 0.0f);
}

TEST_CASE("the reflections of a source in front lean front, and of one behind, behind") {
    //  The first reflections of a source come from near it -- its images in the side walls, floor and
    //  ceiling are all on its side of the listener. That survives the bus: twelve beams, no source.
    //  Catches the virtual sources' taps crossed over, and reflections encoded at the virtual source's
    //  direction instead of the image's.
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(hall(), 6.0);
    CHECK(leaning(impulseFrom(early, 3, {1, 0, 0})).x > 0.0);
    CHECK(leaning(impulseFrom(early, 3, {-1, 0, 0})).x < 0.0);
    CHECK(leaning(impulseFrom(early, 3, {0, 1, 0})).y > 0.0);
    CHECK(leaning(impulseFrom(early, 3, {0, -1, 0})).y < 0.0);
}

TEST_CASE("each reflection arrives from where its image is, not from where its virtual source is") {
    /*  A source a metre in front of a listener in the middle of a hall: the virtual sources that hear it
        are all in front, but its images are all round -- the back wall's is as near as the front wall's.
        So the reflections lean forward hardly at all. How far they should lean is worked out from the
        model, each image's direction weighted by its energy and by how much its virtual source hears of
        the source, and the field has to agree. Encoded from the virtual sources' own directions it would
        lean forward almost entirely. */
    const Room room = hall();
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(room, 1.0);
    auto bus = std::make_unique<BusReflections>();
    busReflections(room, 1.0, *bus);
    const Vec3 front{1, 0, 0};
    Vec3 model{0, 0, 0}, naive{0, 0, 0};
    double total = 0.0;
    for (int v = 0; v < kVirtualSources; ++v) {
        const double w = maxReBeam(3, dot(bus->direction[static_cast<std::size_t>(v)], front));
        for (int i = 0; i < bus->count; ++i) {
            const Reflection& t = bus->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)];
            const double e = w * w * t.mirror * t.mirror;
            model = model + t.direction * e;
            naive = naive + bus->direction[static_cast<std::size_t>(v)] * e;
            total += e;
        }
    }
    model = model * (1.0 / total);
    naive = naive * (1.0 / total);
    const Heard h = impulseFrom(early, 3, front);
    const Vec3 heard = leaning(h) * (1.0 / omniEnergy(h));
    INFO("model ", model.x, " naive ", naive.x, " heard ", heard.x);
    CHECK(naive.x > model.x + 0.4);  // the two differ enough for the test to mean something
    CHECK(length(heard - model) < 0.15);
}

TEST_CASE("what the walls scattered and the mixing time took goes to the tail, not into the field") {
    EarlyReflections early;
    early.prepare(3, kFs);
    const Room smooth = deriveRoom({20.0, RoomShape::Hall, 1.9, 0.5, 0.0}),
               rough = deriveRoom({20.0, RoomShape::Hall, 1.9, 0.5, 1.0});
    const auto energies = [&](const Room& room) {
        early.setRoom(room, 6.0);
        const Heard h = impulseFrom(early, 3, fromAzEl(0.4, 0.2));
        double sc = 0.0;
        for (const float v : h.scattered) sc += static_cast<double>(v) * v;
        return std::pair{omniEnergy(h), sc};
    };
    const auto [mirrorSmooth, tailSmooth] = energies(smooth);
    const auto [mirrorRough, tailRough] = energies(rough);
    CHECK(mirrorRough < 0.5 * mirrorSmooth);  // rough walls leave little that is still a mirror image
    CHECK(tailRough > 1.5 * tailSmooth);      // and send the tail the rest
    CHECK(tailSmooth > 0.0);                  // smooth ones still hand over what the mixing time fades
    CHECK(early.scatteredEnergy() > early.mirrorEnergy());
}

TEST_CASE("a bus of any order is served, from its first sixteen channels") {
    for (const int order : {1, 2, 3, 5}) {
        EarlyReflections early;
        early.prepare(order, kFs);
        early.setRoom(hall(), 6.0);
        const Heard h = impulseFrom(early, order, fromAzEl(0.7, 0.1), 0.25);
        CHECK(omniEnergy(h) > 1e-4);
        //  above order 3 the reflections are placed at order 3: the channels past sixteen stay silent
        for (std::size_t f = 0; f < h.field.size() / static_cast<std::size_t>(h.channels); ++f)
            for (int c = 16; c < h.channels; ++c) REQUIRE(h.field[f * h.channels + c] == 0.0f);
    }
}

TEST_CASE("the reflections do not care how the audio is cut up, and a reset is a fresh start") {
    const int C = 16, frames = 6000;
    std::vector<float> in(static_cast<std::size_t>(C * frames));
    unsigned seed = 8u;
    for (float& v : in) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 4000.0f;
    }
    EarlyReflections early;
    early.prepare(3, kFs);
    early.setRoom(hall(), 6.0);
    const auto render = [&](const std::vector<int>& pieces) {
        std::vector<float> field(in.size()), sc(static_cast<std::size_t>(kVirtualSources * frames));
        early.reset();
        int at = 0;
        std::size_t p = 0;
        while (at < frames) {
            const int n = std::min(frames - at, pieces[p++ % pieces.size()]);
            early.process(in.data() + at * C, field.data() + at * C, sc.data() + at * kVirtualSources, n);
            at += n;
        }
        field.insert(field.end(), sc.begin(), sc.end());
        return field;
    };
    const auto whole = render({frames}), cut = render({1, 7, 64, 333, 1024});
    CHECK(std::memcmp(whole.data(), cut.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE("the late and quiet reflections are encoded at order 1, and what that risks stays small") {
    /*  What it saves goes with a count -- late reflections are the many -- and what it risks goes with
        an energy, so a tap must be both late and quiet to be narrowed. By time alone the cathedral,
        whose few reflections are late and strong, loses 65 % of its second- and third-order energy,
        for a preset that is the cheap one to begin with.
        Catches: the level gate dropped (the cathedral then narrows half its reflection energy), the
        time gate dropped (a room's loud early reflections narrow), and either condition inverted. */
    struct Case {
        const char* name;
        RoomSettings room;
        double distance;
    };
    const Case cases[] = {{"ambience", {4.0, RoomShape::Room, 0.35, 0.50, 0.80}, 1.5},
                          {"hall", {20.0, RoomShape::Hall, 1.9, 0.50, 0.60}, 6.0},
                          {"cathedral", {38.0, RoomShape::Tall, 6.5, 0.35, 0.50}, 14.0}};
    for (const Case& c : cases) {
        EarlyReflections early;
        early.prepare(3, 48000.0);
        early.setRoom(deriveRoom(c.room), c.distance);
        INFO(std::string(c.name), ": ", early.narrowedTaps(), " of ", early.liveTaps(), " narrowed, holding ",
             early.narrowedShare(), " of the reflections' energy");
        //  it must actually bite, or the saving is imaginary
        CHECK(early.narrowedTaps() > early.liveTaps() / 8);
        //  and never reach what is heard as a direction: measured, ambience 0.16 %, the hall 0.96 %,
        //  the cathedral 0.26 % -- where by time alone the cathedral held 67 %
        CHECK(early.narrowedShare() < 0.01);
    }
}
