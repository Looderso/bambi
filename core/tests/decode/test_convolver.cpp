// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/binaural.hpp"
#include "bambi/decode/convolver.hpp"
#include "bambi/decode/decoder.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const HeadModel& shipped() {
    static const HeadModel model = [] {
        HeadModel m;
        std::string error;
        REQUIRE_MESSAGE(parseHeadModel(readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/head.json"), m, &error), error);
        return m;
    }();
    return model;
}

/// Filters for `order` at the sliders, from the shipped head on a small grid.
struct Built {
    BinauralFilterBuilder builder;
    Built(int order, double shift, double size) {
        std::vector<Vec3> dirs;
        std::vector<double> w;
        quadratureGrid(6, 11, dirs, w);
        FilterDesign d;
        d.order = order;
        REQUIRE(builder.prepare(shipped(), d, dirs, w));
        builder.build(shift, size);
    }
};

std::vector<float> noise(int channels, int frames, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(static_cast<std::size_t>(channels * frames));
    for (float& v : x) v = u(rng);
    return x;
}

/// The direct convolution: out[f][e] = sum over channels and taps, in double.
std::vector<double> direct(const BinauralFilterBuilder& b, const std::vector<float>& in, int frames) {
    const int C = b.channels(), T = b.taps();
    std::vector<double> out(static_cast<std::size_t>(frames) * 2, 0.0);
    for (int f = 0; f < frames; ++f)
        for (int e = 0; e < 2; ++e) {
            double acc = 0;
            for (int c = 0; c < C; ++c) {
                const std::span<const float> h = b.filter(c, e);
                for (int t = 0; t < T && t <= f; ++t)
                    acc += static_cast<double>(h[static_cast<std::size_t>(t)]) * in[static_cast<std::size_t>((f - t) * C + c)];
            }
            out[static_cast<std::size_t>(f * 2 + e)] = acc;
        }
    return out;
}

/// Runs `in` through the convolver in pieces of the given sizes, cycling.
std::vector<float> run(BinauralConvolver& conv, const std::vector<float>& in, int frames, const std::vector<int>& cuts) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0f);
    std::size_t cut = 0;
    for (int done = 0; done < frames;) {
        const int n = std::min(cuts[cut++ % cuts.size()], frames - done);
        conv.process(in.data() + static_cast<std::size_t>(done) * static_cast<std::size_t>(conv.channels()),
                     out.data() + static_cast<std::size_t>(done) * 2, n);
        done += n;
    }
    return out;
}
}  // namespace

TEST_CASE("the convolution is the direct one, one partition late, however the audio is cut") {
    //  Catches: the wrong half of the overlap-save result, a history slot off by one partition, a block boundary
    //  that drops or doubles a frame, a partition size the gathering does not follow.
    const Built built(1, 0.4, 1.1);
    const int C = 4, frames = 1200;
    const std::vector<float> in = noise(C, frames, 7);
    const std::vector<double> want = direct(built.builder, in, frames);
    for (int partition : {32, 64, 128}) {
        BinauralConvolver conv;
        REQUIRE(conv.prepare(1, built.builder.taps(), partition));
        CHECK(conv.latency() == partition);
        REQUIRE(conv.publish(built.builder));
        const std::vector<float> got = run(conv, in, frames, {1, 7, 64, 100, 513, 3});
        double worst = 0, peak = 0;
        for (int f = 0; f < frames; ++f)
            for (int e = 0; e < 2; ++e) {
                const double w = f >= partition ? want[static_cast<std::size_t>((f - partition) * 2 + e)] : 0.0;
                worst = std::max(worst, std::abs(got[static_cast<std::size_t>(f * 2 + e)] - w));
                peak = std::max(peak, std::abs(w));
            }
        CHECK_MESSAGE(worst < 2e-5 * peak, "partition " << partition << ": " << worst << " off, peak " << peak);
    }
}

TEST_CASE("silence until a set is published, and a size that does not hold is refused") {
    //  Catches: a convolver that plays an unprepared slot, or accepts a partition the FFT cannot take.
    BinauralConvolver conv;
    CHECK_FALSE(conv.prepare(1, 256, 48));  // not a power of two
    CHECK_FALSE(conv.prepare(1, 256, 16));  // too small for the FFT
    REQUIRE(conv.prepare(1, 256, 64));
    const std::vector<float> in = noise(4, 300, 3);
    std::vector<float> out(600, 1.0f);
    conv.process(in.data(), out.data(), 300);
    for (float v : out) CHECK(v == 0.0f);
    const Built other(2, 0.0, 1.0);
    CHECK_FALSE(conv.publish(other.builder));  // nine channels into a four-channel convolver
}

TEST_CASE("a new set fades in over one partition and then plays alone") {
    //  Catches: a hard swap, a fade over the wrong block, or the old set read after its slot went back.
    const Built a(1, 0.0, 1.0), b(1, 1.5, 1.3);
    const int C = 4, P = 64, frames = 64 * 8;
    const std::vector<float> in = noise(C, frames, 11);
    BinauralConvolver onlyA, onlyB, swapped;
    for (BinauralConvolver* c : {&onlyA, &onlyB, &swapped}) REQUIRE(c->prepare(1, 256, P));
    REQUIRE(onlyA.publish(a.builder));
    REQUIRE(onlyB.publish(b.builder));
    REQUIRE(swapped.publish(a.builder));
    const std::vector<float> ya = run(onlyA, in, frames, {P}), yb = run(onlyB, in, frames, {P});
    std::vector<float> got(static_cast<std::size_t>(frames) * 2);
    const int before = 3 * P;  // three blocks with a, then b is published
    swapped.process(in.data(), got.data(), before);
    REQUIRE(swapped.publish(b.builder));
    swapped.process(in.data() + static_cast<std::size_t>(before * C), got.data() + static_cast<std::size_t>(before) * 2, frames - before);
    // The block gathered after the publish (frames 192..255) is convolved with both sets; its result, faded, plays
    // over the following block (256..319); from then on only b plays.
    double stepAtSwap = 0;
    for (int f = 0; f < frames; ++f)
        for (int e = 0; e < 2; ++e) {
            const std::size_t i = static_cast<std::size_t>(f * 2 + e);
            const int block = f / P;
            float want;
            if (block <= 3) want = ya[i];
            else if (block == 4) {
                const float t = static_cast<float>(f % P + 1) / static_cast<float>(P);
                want = ya[i] * (1.0f - t) + yb[i] * t;
            } else want = yb[i];
            stepAtSwap = std::max(stepAtSwap, static_cast<double>(std::abs(got[i] - want)));
        }
    CHECK(stepAtSwap < 1e-6);
}

TEST_CASE("the decoder builds what was requested, once, and the audio hears exactly a direct build") {
    //  Catches: a stale flag cleared before the sliders are read (a move during a build lost), a worker that
    //  rebuilds when nothing changed, a decoder that plays before anything was built.
    FilterDesign d;
    d.order = 1;
    BinauralDecoder decoder;
    REQUIRE(decoder.prepare(shipped(), d, 64, 6, 11));
    CHECK(decoder.stale());  // the defaults are a request too
    CHECK(decoder.work());
    CHECK_FALSE(decoder.stale());
    CHECK_FALSE(decoder.work());
    decoder.request(0.7, 0.9);
    decoder.request(0.4, 1.1);  // the newer one wins
    CHECK(decoder.work());
    CHECK_FALSE(decoder.work());

    const Built same(1, 0.4, 1.1);
    BinauralConvolver conv;
    REQUIRE(conv.prepare(1, 256, 64));
    REQUIRE(conv.publish(same.builder));
    const int frames = 700;
    const std::vector<float> in = noise(4, frames, 5);
    const std::vector<float> want = run(conv, in, frames, {64});
    std::vector<float> got(static_cast<std::size_t>(frames) * 2);
    decoder.process(in.data(), got.data(), frames);
    for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i] == want[i]);  // bit for bit: the same build, the same path
}
