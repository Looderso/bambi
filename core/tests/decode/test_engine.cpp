// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

#include "bambi/decode/engine.hpp"
#include "bambi/decode/fill.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/decode/orientation.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/legendre.hpp"
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
    channels.reserve(out.size());
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

namespace {

/// An engine prepared at order 1 that has played `blocks` of kFrames with `c`.
void play(DecoderEngine& engine, const DecoderState& d, const DecoderFrame& c, int blocks) {
    std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
    std::array<float*, 2> channels{out[0].data(), out[1].data()};
    const std::vector<float> in = click(1);
    for (int b = 0; b < blocks; ++b) engine.process(d, c, in.data(), channels.data(), 2, kFrames);
}

}  // namespace

TEST_CASE("an axis set back to zero by hand loses what its rate turned, and the other axes keep theirs") {
    //  Catches: the rate's turn kept after a reset, so the scene stays turned until the transport restarts; every
    //  axis cleared by one reset.
    DecoderEngine engine;
    engine.setHeads(shipped(), shipped());
    const DecoderState d;
    REQUIRE(engine.prepare(d, 1, kRate, kFrames, 0.0f));
    DecoderFrame spinning;
    spinning.yawRateDeg = 90.0;
    spinning.pitchRateDeg = 45.0;
    play(engine, d, spinning, 24);  // 0.512 s
    CHECK(engine.turn()[0] == doctest::Approx(46.08));
    CHECK(engine.turn()[1] == doctest::Approx(23.04));

    DecoderFrame reset;  // yaw back to zero by hand, the rates stopped
    reset.zeroTurns = 1;
    play(engine, d, reset, 1);
    CHECK(engine.turn()[0] == doctest::Approx(0.0));
    CHECK(engine.turn()[1] == doctest::Approx(23.04));
}

TEST_CASE("each axis's turn is its own angle plus its rate's, with no tracker in it and no jump past the pole") {
    //  Catches: the turn read back from the whole rotation, so a tracker turning the scene moves the yaw, and a
    //  pitch past 90 degrees reads as yaw and roll turned half round.
    DecoderEngine engine;
    engine.setHeads(shipped(), shipped());
    DecoderState d;
    d.trackerTurnsScene = true;
    REQUIRE(engine.prepare(d, 1, kRate, kFrames, 0.0f));
    DecoderFrame c;
    c.yawDeg = 10.0;
    c.pitchDeg = 80.0;
    c.pitchRateDeg = 40.0;
    c.tracker = headFromYpr(30.0, 0.0, 0.0);
    play(engine, d, c, 24);
    CHECK(engine.turn()[0] == doctest::Approx(10.0));
    CHECK(engine.turn()[1] == doctest::Approx(100.48));
    CHECK(engine.turn()[2] == doctest::Approx(0.0));
}

TEST_CASE("the level starts where prepare puts it, rather than gliding there from unity") {
    //  Catches: the level glide starting at unity, so a render at -60 dB opens with a burst.
    DecoderFrame quiet;
    quiet.levelDb = -60.0f;
    const auto out = render(DecoderState{}, quiet, 2, -60.0f);
    CHECK(energy(out[0]) == 0.0);
    CHECK(energy(out[1]) == 0.0);
}

namespace {

constexpr int kLong = 4096;

/// A laptop's two speakers at one distance on channels 1-2, the gaps filled or not, headphones on 3-4.
DecoderState laptopPair(bool fill) {
    DecoderState d;
    d.speakersPlay = true;
    d.headphoneChannel = 3;
    for (const auto& [az, el] : {std::pair{16.7, -30.4}, std::pair{-17.8, -31.0}}) {
        Loudspeaker s;
        s.azimuthDeg = az;
        s.elevationDeg = el;
        s.radiusM = 2.0;
        s.channel = static_cast<int>(d.layout.size()) + 1;
        d.layout.push_back(s);
    }
    d.fill = fill;
    return d;
}

/// The engine's four channels after an impulse from `from`, at order 3, and the latency it reported.
std::vector<std::vector<float>> impulseThrough(const DecoderState& d, Vec3 from, int& latency) {
    DecoderEngine engine;
    engine.setHeads(shipped(), shipped());
    REQUIRE(engine.prepare(d, 3, kRate, kLong, 0.0f));
    latency = engine.latency();
    const int C = numChannels(3);
    std::vector<float> in(static_cast<std::size_t>(kLong * C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(from, 3, y);
    for (int c = 0; c < C; ++c) in[static_cast<std::size_t>(c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
    std::vector<std::vector<float>> out(4, std::vector<float>(kLong, 0.0f));
    std::vector<float*> channels;
    channels.reserve(out.size());
    for (auto& ch : out) channels.push_back(ch.data());
    engine.process(d, DecoderFrame{}, in.data(), channels.data(), 4, kLong);
    return out;
}

std::size_t peakAt(const std::vector<float>& x) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > std::abs(x[at])) at = i;
    return at;
}

}  // namespace

TEST_CASE("with the gaps filled every output waits the same: the reported latency, the headphones, a loudspeaker") {
    //  Catches: a latency that does not follow the fill, the headphones or the dry part left early so the outputs
    //  no longer land together, the fill added to the other loudspeaker.
    int plainLatency = 0, filledLatency = 0;
    const Vec3 left = direction(16.7, -30.4);
    const auto plain = impulseThrough(laptopPair(false), left, plainLatency);
    const auto filled = impulseThrough(laptopPair(true), left, filledLatency);
    CHECK(plainLatency == DecoderEngine::kPartition);
    CHECK(filledLatency == DecoderEngine::kPartition + 128 + DecoderEngine::kPartition);
    const std::size_t wait = static_cast<std::size_t>(filledLatency - plainLatency);
    for (std::size_t c : {std::size_t{2}, std::size_t{3}}) {  // the headphones: the same, later
        double worst = 0.0;
        for (std::size_t i = 0; i + wait < static_cast<std::size_t>(kLong); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(filled[c][i + wait] - plain[c][i])));
        CHECK(worst < 1e-6);
    }
    //  On its own loudspeaker the source lands as late as everything else, and that loudspeaker carries it.
    CHECK(peakAt(filled[0]) == peakAt(plain[0]) + wait);
    CHECK(10.0 * std::log10(energy(filled[1]) / energy(filled[0])) < -6.0);
}

TEST_CASE("with the gaps filled the loudspeakers play as loud over every direction as without") {
    //  Catches: the fill's loudness left out of the dry part or of the cancellation's feeds.
    const auto loudness = [](bool fill) {
        DecoderState d;
        d.speakersPlay = true;
        d.layout = bs2051Layout("0+5+0")->speakers;
        d.headphoneChannel = 6;
        d.fill = fill;
        DecoderEngine engine;
        engine.setHeads(shipped(), shipped());
        constexpr int kEach = 2048, kPoints = 200, kOut = 7;
        REQUIRE(engine.prepare(d, 3, kRate, kEach, 0.0f));
        const int C = numChannels(3);
        std::vector<float> in(static_cast<std::size_t>(kEach * C));
        std::vector<double> y(static_cast<std::size_t>(C));
        std::vector<std::vector<float>> out(kOut, std::vector<float>(kEach));
        std::vector<float*> channels;
        channels.reserve(out.size());
        for (auto& ch : out) channels.push_back(ch.data());
        double e = 0.0;
        for (int j = 0; j <= kPoints; ++j) {  // one block more, for the last impulse's tail
            std::fill(in.begin(), in.end(), 0.0f);
            if (j < kPoints) {
                const double z = 1.0 - (2.0 * j + 1.0) / kPoints, r = std::sqrt(1.0 - z * z),
                             phi = j * 2.399963229728653;
                shSN3D(Vec3{r * std::cos(phi), r * std::sin(phi), z}, 3, y);
                for (int c = 0; c < C; ++c)
                    in[static_cast<std::size_t>(c)] = static_cast<float>(y[static_cast<std::size_t>(c)]);
            }
            engine.process(d, DecoderFrame{}, in.data(), channels.data(), kOut, kEach);
            for (int o = 0; o < 5; ++o) e += energy(out[static_cast<std::size_t>(o)]);
        }
        return e;
    };
    CHECK(std::abs(10.0 * std::log10(loudness(true) / loudness(false))) < 0.5);
}

TEST_CASE("behind and to the left the fill gives the ears what headphones would give them") {
    //  Catches: the HRTF window, the binaural decode or the cancellation wired the wrong way round, the fill's rows
    //  on the wrong outputs, a delay the ears do not get, the fill's loudness left out of the feeds.
    int latency = 0;
    const DecoderState d = laptopPair(true);
    const Vec3 from = direction(140, 20);  // not on the median plane, so swapped ears or loudspeakers show
    const auto out = impulseThrough(d, from, latency);
    REQUIRE(latency == 256);

    //  The same head the engine built the fill from, at the laptop's two loudspeakers and from the source.
    BinauralFilterBuilder head;
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(12, 20, dirs, w);
    REQUIRE(head.prepare(shipped(), fillFilterDesign(kRate), dirs, w));
    head.build(0.0, 1.0);
    //  What the fill gives the ears, in its band: the source, at the cancellation's level and the fill's loudness.
    Layout layout;
    layout.speakers = d.layout;
    const SpeakerDesign sd = design(layout, 3);
    FillWindows windows;
    fillWindows(coveredRegion(sd), FillSettings{}, 3, windows);
    Cancellation cancel;
    REQUIRE(designCancellation(sd, head, FillSettings{}, kRate, cancel));
    const double gain = cancel.level * fillLoudness(sd, windows, cancel, head, kRate);
    const auto earsFrom = [&](Vec3 v) {
        std::vector<double> y(static_cast<std::size_t>(numChannels(kFillOrder)));
        shSN3D(v, kFillOrder, y);
        std::array<std::vector<double>, 2> e{std::vector<double>(8192), std::vector<double>(8192)};
        for (int ear = 0; ear < 2; ++ear)
            for (int c = 0; c < head.channels(); ++c)
                for (int t = 0; t < head.taps(); ++t)
                    e[static_cast<std::size_t>(ear)][static_cast<std::size_t>(t)] +=
                        y[static_cast<std::size_t>(c)] * head.filter(c, ear)[static_cast<std::size_t>(t)];
        return e;
    };
    RealFft fft;
    REQUIRE(fft.prepare(8192));
    const auto spec = [&](const std::vector<double>& x) {
        std::vector<double> re(fft.bins()), im(fft.bins());
        fft.forward(x, re, im);
        std::vector<std::complex<double>> s(fft.bins());
        for (std::size_t k = 0; k < s.size(); ++k) s[k] = {re[k], im[k]};
        return s;
    };
    const auto padded = [](const std::vector<float>& x) {
        std::vector<double> y(8192, 0.0);
        std::copy(x.begin(), x.end(), y.begin());
        return y;
    };
    const auto L = earsFrom(direction(16.7, -30.4)), R = earsFrom(direction(-17.8, -31.0)), B = earsFrom(from);
    const auto xL = spec(padded(out[0])), xR = spec(padded(out[1]));
    double err = 0.0, ref = 0.0;
    for (int ear = 0; ear < 2; ++ear) {
        const auto hl = spec(L[static_cast<std::size_t>(ear)]), hr = spec(R[static_cast<std::size_t>(ear)]);
        const auto target = spec(B[static_cast<std::size_t>(ear)]);
        for (std::size_t k = 0; k < fft.bins(); ++k) {
            const double hz = static_cast<double>(k) * kRate / 8192.0;
            if (hz < 500.0 || hz > 4000.0) continue;
            const auto late = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) * latency / 8192.0);
            const auto heard = hl[k] * xL[k] + hr[k] * xR[k];
            err += std::norm(heard - gain * target[k] * late);
            ref += std::norm(gain * target[k]);
        }
    }
    CHECK(10.0 * std::log10(err / ref) < -15.0);
}

namespace {

/// Four voices of noise round the listener at order 3, `frames` long, the same every time.
std::vector<float> voices(int frames) {
    const int C = numChannels(3);
    std::vector<float> in(static_cast<std::size_t>(frames * C), 0.0f);
    std::vector<double> y(static_cast<std::size_t>(C));
    unsigned seed = 17u;
    const Vec3 at[4] = {direction(140, 20), direction(-100, 0), direction(10, -30), direction(180, 60)};
    for (const Vec3& v : at) {
        shSN3D(v, 3, y);
        for (int f = 0; f < frames; ++f) {
            seed = seed * 1664525u + 1013904223u;
            const float s = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 4000.0f;
            for (int c = 0; c < C; ++c)
                in[static_cast<std::size_t>(f * C + c)] += s * static_cast<float>(y[static_cast<std::size_t>(c)]);
        }
    }
    return in;
}

/// The engine's four channels over `frames` of voices in blocks of `block`, prepared for `first` and switched to
/// `then` between the blocks at `switchAt` (never, when it is past the end).
std::vector<std::vector<float>> run(const DecoderState& first, const DecoderState& then, int switchAt, int frames,
                                    int block) {
    DecoderEngine engine;
    engine.setHeads(shipped(), shipped());
    REQUIRE(engine.prepare(first, 3, kRate, std::max(block, 1024), 0.0f));
    const int C = numChannels(3);
    const std::vector<float> in = voices(frames);
    std::vector<std::vector<float>> out(4, std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    const DecoderState* d = &first;
    for (int at = 0; at < frames; at += block) {
        if (at == switchAt) {
            engine.build(then);
            d = &then;
        }
        const int n = std::min(block, frames - at);
        float* channels[4];
        for (int c = 0; c < 4; ++c) channels[c] = out[static_cast<std::size_t>(c)].data() + at;
        engine.process(*d, DecoderFrame{}, in.data() + static_cast<std::size_t>(at * C), channels, 4, n);
    }
    return out;
}

}  // namespace

TEST_CASE("switched on mid-render, the fill comes in with its dry part and then plays as if it always had") {
    //  Catches: the fill added before the renderer takes up the design that waits for it -- the dry part early
    //  beside it, a comb -- or a switch that never settles where a render with the fill from the start is.
    const int frames = 8192, at = 2048;
    const auto plain = run(laptopPair(false), laptopPair(false), frames, frames, 512);
    const auto always = run(laptopPair(true), laptopPair(true), frames, frames, 512);
    const auto switched = run(laptopPair(false), laptopPair(true), at, frames, 512);
    for (std::size_t c : {std::size_t{0}, std::size_t{1}}) {
        double early = 0.0, late = 0.0;
        //  while the new design's lines fill, what plays is the old design alone
        for (int i = at; i < at + 256; ++i)
            early = std::max(early, static_cast<double>(std::abs(switched[c][static_cast<std::size_t>(i)] -
                                                                 plain[c][static_cast<std::size_t>(i)])));
        for (int i = frames - 1024; i < frames; ++i)
            late = std::max(late, static_cast<double>(std::abs(switched[c][static_cast<std::size_t>(i)] -
                                                               always[c][static_cast<std::size_t>(i)])));
        CHECK(early < 1e-6);
        CHECK(late < 1e-5);
    }
    for (std::size_t c : {std::size_t{2}, std::size_t{3}}) {  // the headphones settle at the same wait
        double late = 0.0;
        for (int i = frames - 1024; i < frames; ++i)
            late = std::max(late, static_cast<double>(std::abs(switched[c][static_cast<std::size_t>(i)] -
                                                               always[c][static_cast<std::size_t>(i)])));
        CHECK(late < 1e-5);
    }
}

TEST_CASE("with the gaps filled the output does not depend on how the host cuts its blocks") {
    //  Catches: the fill, its gain or the headphones' wait advancing per block rather than per frame.
    const int frames = 4096;
    const auto ref = run(laptopPair(true), laptopPair(true), frames, frames, 1024);
    for (const int block : {1, 100}) {
        CAPTURE(block);
        const auto got = run(laptopPair(true), laptopPair(true), frames, frames, block);
        double worst = 0.0;
        for (std::size_t c = 0; c < 4; ++c)
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames); ++i)
                worst = std::max(worst, static_cast<double>(std::abs(got[c][i] - ref[c][i])));
        CHECK(worst == 0.0);
    }
}

TEST_CASE("while the gaps are filled a pair plays its fold, whatever its slice switch says") {
    //  Catches: the slice's narrow microphones under the fill, a combination nobody tuned, which the page shows
    //  greyed on the fold.
    DecoderState sliced = laptopPair(true);
    sliced.pairSlice = true;
    int a = 0, b = 0;
    const auto fold = impulseThrough(laptopPair(true), direction(140, 20), a);
    const auto withSlice = impulseThrough(sliced, direction(140, 20), b);
    double worst = 0.0;
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t i = 0; i < static_cast<std::size_t>(kLong); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(withSlice[c][i] - fold[c][i])));
    CHECK(worst == 0.0);
}
