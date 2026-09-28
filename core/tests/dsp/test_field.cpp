// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstring>
#include <vector>

#include "bambi/dsp/field.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

//  A field with something different in every channel of every frame, from a generator written out
//  here so the numbers are the same on every machine.
std::vector<float> noiseField(int order, int frames, unsigned seed = 1u) {
    std::vector<float> f(static_cast<std::size_t>(numChannels(order) * frames));
    unsigned s = seed;
    for (float& v : f) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(s >> 8) % 20001 - 10000) / 10000.0f;
    }
    return f;
}

struct Turn {
    std::vector<double> blocks;
    std::vector<float> forward, inverse;
    Turn(const Mat3& r, int order)
        : blocks(static_cast<std::size_t>(rotationSize(order))), forward(blocks.size()), inverse(blocks.size()) {
        shRotation(r, order, blocks);
        fieldBlocks(blocks, order, forward, inverse);
    }
};

}  // namespace

TEST_CASE("the float kernel turns a field as the double reference does, at fixed and at run-time widths") {
    //  Orders 1 to 7 take the kernels whose sizes are template parameters; 9 takes the loop that reads
    //  them. Catches a block read row-major, and a fixed kernel given the wrong width or offset.
    const Mat3 r = rotationAbout({0.3, -1.0, 0.6}, 1.234);
    for (const int order : {0, 1, 2, 3, 5, 7, 9}) {
        const int ch = numChannels(order), frames = 5;
        const Turn turn(r, order);
        const auto in = noiseField(order, frames);
        std::vector<float> out(in.size());
        rotateField(turn.forward, order, in.data(), out.data(), frames);
        std::vector<double> frame(static_cast<std::size_t>(ch)), want(frame.size());
        double worst = 0.0;
        for (int f = 0; f < frames; ++f) {
            for (int c = 0; c < ch; ++c) frame[static_cast<std::size_t>(c)] = in[static_cast<std::size_t>(f * ch + c)];
            rotateSH(turn.blocks, order, frame, want);
            for (int c = 0; c < ch; ++c)
                worst = std::max(
                    worst, std::abs(want[static_cast<std::size_t>(c)] - out[static_cast<std::size_t>(f * ch + c)]));
        }
        INFO("order ", order, " worst ", worst);
        CHECK(worst < 5e-6);
    }
}

TEST_CASE("the inverse blocks turn a field back") {
    const Turn turn(rotationAbout({-0.8, 0.1, -0.5}, 2.2), 7);
    const auto in = noiseField(7, 3);
    std::vector<float> there(in.size()), back(in.size());
    rotateField(turn.forward, 7, in.data(), there.data(), 3);
    rotateField(turn.inverse, 7, there.data(), back.data(), 3);
    double moved = 0.0, worst = 0.0;
    for (std::size_t k = 0; k < in.size(); ++k) {
        moved = std::max(moved, static_cast<double>(std::abs(there[k] - in[k])));
        worst = std::max(worst, static_cast<double>(std::abs(back[k] - in[k])));
    }
    CHECK(moved > 0.1);   // it went somewhere
    CHECK(worst < 1e-5);  // and came back
}

TEST_CASE("a spin about the pole is the general turn about z, for a fraction of the work") {
    const int order = 7, frames = 4;
    const double angle = 0.83;
    const Turn turn(rotationAbout({0, 0, 1}, angle), order);
    std::vector<float> c(order + 1), s(order + 1);
    for (int m = 0; m <= order; ++m) {
        c[static_cast<std::size_t>(m)] = static_cast<float>(std::cos(m * angle));
        s[static_cast<std::size_t>(m)] = static_cast<float>(std::sin(m * angle));
    }
    const auto in = noiseField(order, frames);
    std::vector<float> general(in.size()), spun = in;
    rotateField(turn.forward, order, in.data(), general.data(), frames);
    spinField(c, s, order, spun.data(), frames);
    double worst = 0.0;
    for (std::size_t k = 0; k < in.size(); ++k)
        worst = std::max(worst, static_cast<double>(std::abs(general[k] - spun[k])));
    CHECK(worst < 5e-6);  // catches the spin turning the other way, and its pair of channels swapped
}

TEST_CASE("one gain per order reaches every channel of that order and no other") {
    const int order = 5, frames = 3, ch = numChannels(order);
    const std::vector<float> gains{1.0f, 0.5f, 0.25f, 2.0f, 0.0f, -1.0f};
    const auto in = noiseField(order, frames);
    auto out = in;
    orderGains(gains, order, out.data(), frames);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) {
            const auto k = static_cast<std::size_t>(f * ch + c);
            CHECK(out[k] == in[k] * gains[static_cast<std::size_t>(acnOrder(c))]);
        }
}

TEST_CASE("every kernel gives the same bits for one call of many frames as for many calls of one") {
    //  What block-size independence rests on: nothing here may depend on how many frames it was given.
    const int order = 7, frames = 97, ch = numChannels(order);
    const Turn turn(rotationAbout({0.2, 0.9, -0.4}, 0.7), order);
    std::vector<float> c(order + 1), s(order + 1), gains(order + 1);
    for (int m = 0; m <= order; ++m) {
        c[static_cast<std::size_t>(m)] = static_cast<float>(std::cos(m * 0.3));
        s[static_cast<std::size_t>(m)] = static_cast<float>(std::sin(m * 0.3));
        gains[static_cast<std::size_t>(m)] = 1.0f / static_cast<float>(m + 1);
    }
    const auto in = noiseField(order, frames);
    const auto run = [&](const std::vector<int>& pieces) {
        std::vector<float> out(in.size());
        int at = 0;
        for (const int n : pieces) {
            rotateField(turn.forward, order, in.data() + at * ch, out.data() + at * ch, n);
            spinField(c, s, order, out.data() + at * ch, n);
            orderGains(gains, order, out.data() + at * ch, n);
            at += n;
        }
        return out;
    };
    const auto whole = run({frames});
    const auto ones = run(std::vector<int>(frames, 1));
    const auto uneven = run({1, 31, 2, 63});
    CHECK(std::memcmp(whole.data(), ones.data(), whole.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(whole.data(), uneven.data(), whole.size() * sizeof(float)) == 0);
}
