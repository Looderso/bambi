// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/virtualarray.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

TEST_CASE("the field design encodes each loudspeaker's row at its direction, at one distance") {
    //  Catches: a loudspeaker encoded at another's direction, the distance gain or delay carried along, an
    //  imaginary point encoded.
    const SpeakerDesign array = design(*bs2051Layout("4+7+0"), 3);
    const SpeakerDesign field = fieldDesign(array, 7);
    const int C = 16, F = 64;
    CHECK(field.outputs == F);
    CHECK(field.order == 3);
    CHECK(field.channel.empty());
    CHECK(field.delaySeconds.empty());
    // A plane wave from the front loudspeaker's direction, through the array: its field is the loudspeakers' gains
    // encoded at their directions. The omni channel is their plain sum; the front loudspeaker takes most of it.
    std::vector<double> y(static_cast<std::size_t>(C)), gain(static_cast<std::size_t>(array.outputs), 0.0);
    shSN3D({1, 0, 0}, 3, y);
    for (int o = 0; o < array.outputs; ++o)
        for (int c = 0; c < C; ++c)
            gain[static_cast<std::size_t>(o)] +=
                array.matrix[static_cast<std::size_t>(o * C + c)] * y[static_cast<std::size_t>(c)];
    double omni = 0, sum = 0;
    for (int c = 0; c < C; ++c) omni += field.matrix[static_cast<std::size_t>(c)] * y[static_cast<std::size_t>(c)];
    for (double g : gain) sum += g;
    CHECK(omni == doctest::Approx(sum).epsilon(1e-9));
    CHECK(gain[2] > 0.4 * sum);  // the centre loudspeaker, channel 3 in the recommendation's order
    for (int o = 0; o < array.outputs; ++o) CHECK(gain[2] >= gain[static_cast<std::size_t>(o)]);
    // The field's x-dipole (ACN 3) is the sum weighted by each loudspeaker's x: all front-heavy here.
    double dipole = 0;
    for (int c = 0; c < C; ++c)
        dipole += field.matrix[static_cast<std::size_t>(3 * C + c)] * y[static_cast<std::size_t>(c)];
    CHECK(dipole > 0.5 * sum);
}

TEST_CASE("the virtual array is the field renderer and the order-7 binaural path, one partition late") {
    //  Catches: a chunk boundary that loses frames between the two stages, or the field at the wrong order.
    const double fs = 48000.0;
    const SpeakerDesign array = design(*bs2051Layout("4+7+0"), 3);
    VirtualArray va;
    REQUIRE(va.prepare(shipped(), 3, fs, 64));
    CHECK(va.latency() == 64);
    CHECK(va.fieldOrder() == 7);
    REQUIRE(va.publishLayout(array));
    va.buildHead(0.2, 1.05);

    SpeakerRenderer renderer;
    REQUIRE(renderer.prepare(3, 64, fs, 0.0));
    REQUIRE(renderer.publish(fieldDesign(array, 7)));
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(12, 20, dirs, w);
    FilterDesign fd;
    fd.order = 7;
    BinauralFilterBuilder builder;
    REQUIRE(builder.prepare(shipped(), fd, dirs, w));
    builder.build(0.2, 1.05);
    BinauralConvolver convolver;
    REQUIRE(convolver.prepare(7, fd.taps, 64));
    REQUIRE(convolver.publish(builder));

    const int frames = 777;
    const std::vector<float> in = noise(16, frames, 8);
    std::vector<float> got(static_cast<std::size_t>(frames) * 2), want(got.size()),
        field(static_cast<std::size_t>(frames) * 64);
    for (int done = 0, cut = 0; done < frames; ++cut) {  // odd cuts through the virtual array
        const int n = std::min(cut % 2 ? 100 : 37, frames - done);
        va.process(in.data() + static_cast<std::size_t>(done) * 16, 16, got.data() + static_cast<std::size_t>(done) * 2,
                   n);
        done += n;
    }
    renderer.process(in.data(), 16, field.data(), 64, frames);
    convolver.process(field.data(), want.data(), frames);
    double worst = 0;
    for (std::size_t i = 0; i < got.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(got[i] - want[i])));
    CHECK(worst < 1e-6);  // the same two stages; only the chunking differs
}

TEST_CASE("a source in front of a symmetric array reaches both ears alike") {
    //  Catches: a loudspeaker encoded at a mirrored direction, or the ears swapped between the stages.
    const SpeakerDesign array = design(*bs2051Layout("4+7+0"), 3);
    VirtualArray va;
    REQUIRE(va.prepare(shipped(), 3, 48000.0, 64));
    REQUIRE(va.publishLayout(array));
    va.buildHead(0.0, 1.0);
    const int frames = 2048;
    std::vector<float> in(static_cast<std::size_t>(frames) * 16, 0.0f), out(static_cast<std::size_t>(frames) * 2);
    std::vector<double> y(16);
    shSN3D({1, 0, 0}, 3, y);
    std::mt19937 rng(2);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int f = 0; f < frames; ++f) {
        const float s = u(rng);
        for (int c = 0; c < 16; ++c)
            in[static_cast<std::size_t>(f * 16 + c)] = s * static_cast<float>(y[static_cast<std::size_t>(c)]);
    }
    va.process(in.data(), 16, out.data(), frames);
    double same = 0, total = 0;
    for (int f = 0; f < frames; ++f) {
        const float l = out[static_cast<std::size_t>(f * 2)], r = out[static_cast<std::size_t>(f * 2 + 1)];
        same += (l - r) * (l - r);
        total += l * l + r * r;
    }
    REQUIRE(total > 0);
    CHECK(same / total < 1e-8);
}

TEST_CASE("the virtual array turns its field against the head, between its two stages") {
    //  Catches: the turn applied to the bus instead of the field, or left out of the chain.
    const double fs = 48000.0;
    const SpeakerDesign array = design(*bs2051Layout("4+7+0"), 3);
    const Mat3 head = rotationAbout({0, 0, 1}, 90.0 * kDeg2Rad);
    VirtualArray va;
    REQUIRE(va.prepare(shipped(), 3, fs, 64));
    REQUIRE(va.publishLayout(array));
    va.buildHead(0.0, 1.0);
    va.setOrientation(head);

    SpeakerRenderer renderer;
    REQUIRE(renderer.prepare(3, 64, fs, 0.0));
    REQUIRE(renderer.publish(fieldDesign(array, 7)));
    HeadRotation turn;
    REQUIRE(turn.prepare(7, fs));
    turn.setOrientation(head);
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(12, 20, dirs, w);
    FilterDesign fd;
    fd.order = 7;
    BinauralFilterBuilder builder;
    REQUIRE(builder.prepare(shipped(), fd, dirs, w));
    builder.build(0.0, 1.0);
    BinauralConvolver convolver;
    REQUIRE(convolver.prepare(7, fd.taps, 64));
    REQUIRE(convolver.publish(builder));

    const int frames = 4096;
    const std::vector<float> in = noise(16, frames, 12);
    std::vector<float> got(static_cast<std::size_t>(frames) * 2), want(got.size()),
        field(static_cast<std::size_t>(frames) * 64), turned(field.size());
    va.process(in.data(), 16, got.data(), frames);
    renderer.process(in.data(), 16, field.data(), 64, frames);
    turn.process(field.data(), turned.data(), frames);
    convolver.process(turned.data(), want.data(), frames);
    double worst = 0, energy = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(got[i] - want[i])));
        energy += want[i] * want[i];
    }
    REQUIRE(energy > 0);
    CHECK(worst < 1e-6);
}
