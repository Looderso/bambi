// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/allrad.hpp"
#include "bambi/decode/layoutfile.hpp"
#include "bambi/decode/speakers.hpp"
#include "bambi/math/sh.hpp"
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

/// The array as the room has it: channel 18 imaginary.
Layout array16() {
    Layout l;
    std::string error;
    REQUIRE_MESSAGE(parseIemLayout(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16.json"), l, &error), error);
    for (Loudspeaker& s : l.speakers)
        if (s.channel == 18) s.imaginary = true;
    return l;
}

SpeakerDesign design(const Layout& l, int order, Weighting w) {
    SpeakerDesign d;
    std::string error;
    REQUIRE_MESSAGE(designAllrad(l, order, w, d, &error), error);
    return d;
}

std::vector<float> noise(int channels, int frames, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(static_cast<std::size_t>(channels * frames));
    for (float& v : x) v = u(rng);
    return x;
}

/// What the design says: row times frame, the gain in, delayed by its whole samples, in double.
std::vector<double> direct(const SpeakerDesign& d, const std::vector<float>& in, int frames, double fs) {
    const int C = numChannels(d.order);
    std::vector<double> gain(static_cast<std::size_t>(d.outputs), 1.0);
    std::vector<int> delay(static_cast<std::size_t>(d.outputs), 0);
    for (std::size_t i = 0; i < d.channel.size(); ++i) {
        gain[static_cast<std::size_t>(d.channel[i])] = d.gain[i];
        delay[static_cast<std::size_t>(d.channel[i])] = static_cast<int>(std::lround(d.delaySeconds[i] * fs));
    }
    std::vector<double> out(static_cast<std::size_t>(frames * d.outputs), 0.0);
    for (int f = 0; f < frames; ++f)
        for (int o = 0; o < d.outputs; ++o) {
            const int src = f - delay[static_cast<std::size_t>(o)];
            if (src < 0) continue;
            double acc = 0;
            for (int c = 0; c < C; ++c)
                acc += d.matrix[static_cast<std::size_t>(o * C + c)] * in[static_cast<std::size_t>(src * C + c)];
            out[static_cast<std::size_t>(f * d.outputs + o)] = acc * gain[static_cast<std::size_t>(o)];
        }
    return out;
}

std::vector<float> run(SpeakerRenderer& r, const std::vector<float>& in, int inChannels, int outChannels, int frames,
                       const std::vector<int>& cuts) {
    std::vector<float> out(static_cast<std::size_t>(frames * outChannels), 0.0f);
    std::size_t cut = 0;
    for (int done = 0; done < frames;) {
        const int n = std::min(cuts[cut++ % cuts.size()], frames - done);
        r.process(in.data() + static_cast<std::size_t>(done * inChannels), inChannels,
                  out.data() + static_cast<std::size_t>(done * outChannels), outChannels, n);
        done += n;
    }
    return out;
}
}  // namespace

TEST_CASE("the renderer plays the design: matrix, gain and whole-sample delay, however the audio is cut") {
    //  Catches: a transposed matrix, the gain on the wrong row, a delay that leads instead of lags, a block boundary
    //  that drops a frame.
    const SpeakerDesign d = design(array16(), 3, Weighting::MaxRe);
    const double fs = 48000.0;
    const int C = 16, frames = 1000;
    const std::vector<float> in = noise(C, frames, 3);
    const std::vector<double> want = direct(d, in, frames, fs);
    SpeakerRenderer r;
    REQUIRE(r.prepare(3, 24, fs));
    REQUIRE(r.publish(d));
    const std::vector<float> got = run(r, in, C, d.outputs, frames, {1, 5, 64, 100, 300});
    double worst = 0;
    for (std::size_t i = 0; i < want.size(); ++i) worst = std::max(worst, std::abs(got[i] - want[i]));
    CHECK(worst < 1e-5);
    // The farthest loudspeaker (channel 1) is not delayed, channel 7 by (1.65 - 1.39) / 343 s = 36 samples.
    CHECK(std::lround(d.delaySeconds[5] * fs) == 36);
}

TEST_CASE("silence before a design, and a design that does not fit is refused") {
    //  Catches: a renderer that plays an empty slot, or writes past the rows or delay lines it made room for.
    SpeakerRenderer r;
    REQUIRE(r.prepare(1, 8, 48000.0, 0.01));
    const std::vector<float> in = noise(4, 200, 1);
    std::vector<float> out(200 * 8, 1.0f);
    r.process(in.data(), 4, out.data(), 8, 200);
    for (float v : out) CHECK(v == 0.0f);
    SpeakerDesign d = design(array16(), 1, Weighting::Basic);
    CHECK_FALSE(r.publish(d));  // 17 outputs into 8
    REQUIRE(r.prepare(1, 17, 48000.0, 0.0001));
    CHECK_FALSE(r.publish(d));  // the delays reach 0.76 ms
    REQUIRE(r.prepare(1, 17, 48000.0));
    CHECK(r.publish(d));
    CHECK_FALSE(r.publish(design(array16(), 2, Weighting::Basic)));  // order 2 into a renderer made for order 1
}

TEST_CASE("a new design fades in over ten milliseconds and then plays alone") {
    //  Catches: a hard swap, a fade that forgets the delays, the old design read after its slot went back.
    const Layout l = array16();
    const SpeakerDesign a = design(l, 3, Weighting::Basic), b = design(l, 3, Weighting::MaxRe);
    const double fs = 48000.0;
    const int C = 16, frames = 2000;
    const std::vector<float> in = noise(C, frames, 9);
    SpeakerRenderer onlyA, onlyB, swapped;
    for (SpeakerRenderer* r : {&onlyA, &onlyB, &swapped}) REQUIRE(r->prepare(3, 17, fs));
    REQUIRE(onlyA.publish(a));
    REQUIRE(onlyB.publish(b));
    REQUIRE(swapped.publish(a));
    const std::vector<float> ya = run(onlyA, in, C, 17, frames, {128}), yb = run(onlyB, in, C, 17, frames, {128});
    std::vector<float> got(static_cast<std::size_t>(frames * 17));
    const int before = 512, fade = swapped.fadeFrames();
    REQUIRE(fade == 480);
    swapped.process(in.data(), C, got.data(), 17, before);
    REQUIRE(swapped.publish(b));
    swapped.process(in.data() + static_cast<std::size_t>(before * C), C, got.data() + static_cast<std::size_t>(before * 17), 17,
                    frames - before);
    // The ramp waits for the new design's longest delay (channel 13 at 1.30 m: 49 samples), then the output is the
    // two designs' mixed with the ramp, after it b alone.
    const int wait = 49;
    double worst = 0;
    for (int f = 0; f < frames; ++f)
        for (int o = 0; o < 17; ++o) {
            const std::size_t i = static_cast<std::size_t>(f * 17 + o);
            float want;
            if (f < before + wait) want = ya[i];
            else if (f < before + wait + fade) {
                const float t = static_cast<float>(f - before - wait + 1) / static_cast<float>(fade);
                want = ya[i] * (1.0f - t) + yb[i] * t;
            } else want = yb[i];
            worst = std::max(worst, static_cast<double>(std::abs(got[i] - want)));
        }
    CHECK(worst < 1e-5);
}

TEST_CASE("a change of delays fades without a step") {
    //  Catches: a delay that jumps to its new value while the matrices fade.
    Layout near = array16(), far = array16();
    for (Loudspeaker& s : far.speakers)
        if (s.channel == 3) s.radiusM = 1.0;  // the front loudspeaker moved 65 cm closer: 91 samples more delay
    const SpeakerDesign a = design(near, 1, Weighting::Basic), b = design(far, 1, Weighting::Basic);
    SpeakerRenderer r;
    REQUIRE(r.prepare(1, 17, 48000.0));
    REQUIRE(r.publish(a));
    const int frames = 3000;
    std::vector<float> in(static_cast<std::size_t>(frames * 4), 0.0f);
    for (int f = 0; f < frames; ++f) in[static_cast<std::size_t>(f * 4)] = 1.0f;  // a steady omni signal
    std::vector<float> out(static_cast<std::size_t>(frames * 17));
    r.process(in.data(), 4, out.data(), 17, 1000);
    REQUIRE(r.publish(b));
    r.process(in.data() + 4000, 4, out.data() + 17000, 17, frames - 1000);
    const int o = 2;  // channel 3
    const float lo = std::min(out[static_cast<std::size_t>(999 * 17 + o)], out[static_cast<std::size_t>(2999 * 17 + o)]);
    const float hi = std::max(out[static_cast<std::size_t>(999 * 17 + o)], out[static_cast<std::size_t>(2999 * 17 + o)]);
    REQUIRE(hi - lo > 0.01f);  // the gain did change: 1.65 m against 1.0 m
    float biggestStep = 0;
    for (int f = 1000; f < frames; ++f) {
        const float v = out[static_cast<std::size_t>(f * 17 + o)], prev = out[static_cast<std::size_t>((f - 1) * 17 + o)];
        CHECK(v >= lo - 1e-6f);
        CHECK(v <= hi + 1e-6f);
        biggestStep = std::max(biggestStep, std::abs(v - prev));
    }
    CHECK(biggestStep < (hi - lo) / 100.0f);  // spread over 480 frames, never in one
}

TEST_CASE("a design that follows a fade closely waits until the lines have settled") {
    //  Catches: the leaving lines read before they hold the previous design throughout, which steps back to the one
    //  before it for the first samples of the next fade.
    Layout near = array16(), far = array16();
    for (Loudspeaker& s : far.speakers)
        if (s.channel == 3) s.radiusM = 1.0;
    const SpeakerDesign a = design(near, 3, Weighting::Basic), b = design(near, 3, Weighting::MaxRe),
                        c = design(far, 3, Weighting::Basic);
    const double fs = 48000.0;
    const int C = 16, frames = 3000, O = 17;
    const std::vector<float> in = noise(C, frames, 21);
    SpeakerRenderer onlyB, onlyC, swapped;
    for (SpeakerRenderer* r : {&onlyB, &onlyC, &swapped}) REQUIRE(r->prepare(3, O, fs));
    REQUIRE(onlyB.publish(b));
    REQUIRE(onlyC.publish(c));
    const std::vector<float> yb = run(onlyB, in, C, O, frames, {64}), yc = run(onlyC, in, C, O, frames, {64});
    std::vector<float> got(static_cast<std::size_t>(frames * O));
    auto play = [&](int from, int to) {
        for (int f = from; f < to; ++f)  // a frame at a time, so a design is taken up as soon as the renderer allows
            swapped.process(in.data() + static_cast<std::size_t>(f * C), C, got.data() + static_cast<std::size_t>(f * O), O, 1);
    };
    const int fade = swapped.fadeFrames(), waitB = 49, waitC = 91, settleB = 49;
    REQUIRE(swapped.publish(a));
    play(0, 500);
    REQUIRE(swapped.publish(b));
    const int endB = 500 + waitB + fade;
    play(500, endB);
    REQUIRE(swapped.publish(c));  // one frame after the fade to b ended
    play(endB, frames);
    // b plays alone until its lines have settled; then c's wait, c's ramp, c alone.
    const int startC = endB + settleB, rampC = startC + waitC;
    double worst = 0;
    for (int f = endB; f < frames; ++f)
        for (int o = 0; o < O; ++o) {
            const std::size_t i = static_cast<std::size_t>(f * O + o);
            float want;
            if (f < rampC) want = yb[i];
            else if (f < rampC + fade) {
                const float t = static_cast<float>(f - rampC + 1) / static_cast<float>(fade);
                want = yb[i] * (1.0f - t) + yc[i] * t;
            } else want = yc[i];
            worst = std::max(worst, static_cast<double>(std::abs(got[i] - want)));
        }
    CHECK(worst < 1e-5);
}

TEST_CASE("the loudspeaker decoder designs what was requested and the audio hears a direct publish") {
    //  Catches: a request lost during a design, a refused layout silencing the output, a stale flag never cleared.
    SpeakerDecoder dec;
    REQUIRE(dec.prepare(3, 17, 48000.0));
    CHECK_FALSE(dec.stale());
    CHECK_FALSE(dec.work());
    dec.request(array16(), 3, Weighting::Basic);
    dec.request(array16(), 3, Weighting::MaxRe);  // the newer one wins
    CHECK(dec.stale());
    CHECK(dec.work());
    CHECK_FALSE(dec.stale());
    CHECK(dec.lastError().empty());
    CHECK(dec.design().outputs == 17);

    SpeakerRenderer r;
    REQUIRE(r.prepare(3, 17, 48000.0));
    REQUIRE(r.publish(design(array16(), 3, Weighting::MaxRe)));
    const int frames = 600;
    const std::vector<float> in = noise(16, frames, 4);
    std::vector<float> want(static_cast<std::size_t>(frames * 17)), got(want.size());
    r.process(in.data(), 16, want.data(), 17, frames);
    dec.process(in.data(), 16, got.data(), 17, frames);
    CHECK(got == want);  // bit for bit: the same design, the same path

    Layout front;
    for (double az : {-30.0, 0.0, 30.0}) {
        Loudspeaker s;
        s.azimuthDeg = az;
        s.channel = static_cast<int>(front.speakers.size()) + 1;
        front.speakers.push_back(s);
    }
    dec.request(front, 3, Weighting::Basic);
    CHECK_FALSE(dec.work());  // refused: the ring leaves a gap wider than a half turn
    CHECK_FALSE(dec.stale());
    CHECK_FALSE(dec.lastError().empty());
    CHECK(dec.design().outputs == 17);  // the array keeps playing
    dec.process(in.data(), 16, got.data(), 17, frames);
    r.process(in.data(), 16, want.data(), 17, frames);
    CHECK(got == want);
}
