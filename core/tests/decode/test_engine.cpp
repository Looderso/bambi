// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <vector>

#include "bambi/decode/engine.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

namespace {

constexpr double kRate = 48000.0;
constexpr int kFrames = 1024;

/// A plane wave from the front at the first frame, at `order`, interleaved.
std::vector<float> click(int order) {
    const int C = numChannels(order);
    std::vector<float> in(static_cast<std::size_t>(kFrames * C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D({1, 0, 0}, order, y);
    for (int c = 0; c < C; ++c)
        in[static_cast<std::size_t>(c)] = static_cast<float>(0.5 * y[static_cast<std::size_t>(c)]);
    return in;
}

/// The bus after one render of kFrames, every channel filled with 1 beforehand so a channel left unwritten shows.
std::vector<std::vector<float>> render(const DecoderState& d, const DecoderFrame& c, int bus, float levelDb = 0.0f) {
    DecoderEngine engine;
    engine.setHeads(shipped(), shipped());
    REQUIRE(engine.prepare(d, 1, kRate, kFrames, levelDb));
    std::vector<std::vector<float>> out(static_cast<std::size_t>(bus), std::vector<float>(kFrames, 1.0f));
    std::vector<float*> channels;
    for (auto& ch : out) channels.push_back(ch.data());
    const std::vector<float> in = click(1);
    engine.process(d, c, in.data(), channels.data(), bus, kFrames);
    return out;
}

double energy(const std::vector<float>& x) {
    double e = 0.0;
    for (float s : x) e += static_cast<double>(s) * s;
    return e;
}

/// A pair of loudspeakers on bus channels 4-5, beside the headphones on 1-2.
DecoderState bothOutputs() {
    DecoderState d;
    d.speakersPlay = true;
    d.layout = bs2051Layout("0+2+0")->speakers;
    d.speakerFirstChannel = 4;
    return d;
}

}  // namespace

TEST_CASE("a prepared engine plays the state from its first block, with nothing left to build") {
    //  Catches: the binaural filters left for a worker after prepare, so a render starts in silence.
    const auto out = render(DecoderState{}, DecoderFrame{}, 2);
    CHECK(energy(out[0]) > 1e-4);
    CHECK(energy(out[1]) > 1e-4);
}

TEST_CASE("each output plays on its own channels, and every other channel is silent") {
    //  Catches: the layout not moved as a block to its first channel, the headphones' pair off by one, a channel no
    //  output feeds left with what the host had in it.
    const auto out = render(bothOutputs(), DecoderFrame{}, 7);
    for (int c : {0, 1, 3, 4}) CHECK(energy(out[static_cast<std::size_t>(c)]) > 1e-6);
    for (int c : {2, 5, 6}) CHECK(energy(out[static_cast<std::size_t>(c)]) == 0.0);
}

TEST_CASE("held, the loudspeakers' feeds stop and the headphones play on") {
    //  Catches: the hold ignored, or holding the headphones too.
    DecoderFrame held;
    held.speakersHeld = true;
    const auto out = render(bothOutputs(), held, 7);
    CHECK(energy(out[0]) > 1e-6);
    CHECK(energy(out[3]) == 0.0);
    CHECK(energy(out[4]) == 0.0);
}

TEST_CASE("the level starts where prepare puts it, rather than gliding there from unity") {
    //  Catches: the level glide starting at unity, so a render at -60 dB opens with a burst.
    DecoderFrame quiet;
    quiet.levelDb = -60.0f;
    const auto out = render(DecoderState{}, quiet, 2, -60.0f);
    CHECK(energy(out[0]) == 0.0);
    CHECK(energy(out[1]) == 0.0);
}
