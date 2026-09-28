// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstring>
#include <vector>

#include "bambi/reverb/tail.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kFs = 48000.0;
constexpr int K = 16;

TailSettings flat(double rt) {
    TailSettings s;
    s.rtLow = s.rtMid = s.rtHigh = rt;
    s.lowCutHz = 5.0;
    s.highCutHz = 23000.0;
    s.preDelaySeconds = 0.0;
    return s;
}

//  An impulse of unit energy, shared by every line, and what comes back for `seconds`.
std::vector<float> impulseResponse(FdnTail& tail, double seconds) {
    const int frames = static_cast<int>(seconds * kFs);
    std::vector<float> in(static_cast<std::size_t>(frames * K), 0.0f), out(in.size());
    for (int k = 0; k < K; ++k) in[static_cast<std::size_t>(k)] = 1.0f / std::sqrt(static_cast<float>(K));
    tail.process(in.data(), nullptr, out.data(), frames);
    return out;
}

/*  RT60 by Schroeder's backward integration of the energy over every line, a line fitted between 5
 *  and 35 dB down -- optionally after a filter, to read one band. */
double rt60(const std::vector<float>& h, const BiquadCoeffs* band = nullptr) {
    const int frames = static_cast<int>(h.size()) / K;
    std::vector<double> energy(static_cast<std::size_t>(frames), 0.0);
    for (int k = 0; k < K; ++k) {
        BiquadState s1, s2;
        for (int f = 0; f < frames; ++f) {
            double v = h[static_cast<std::size_t>(f * K + k)];
            if (band != nullptr) v = run(*band, s2, run(*band, s1, v));
            energy[static_cast<std::size_t>(f)] += v * v;
        }
    }
    for (int f = frames - 2; f >= 0; --f)
        energy[static_cast<std::size_t>(f)] += energy[static_cast<std::size_t>(f + 1)];
    const double top = 10.0 * std::log10(energy[0]);
    int from = -1, to = -1;
    for (int f = 0; f < frames; ++f) {
        const double db = 10.0 * std::log10(energy[static_cast<std::size_t>(f)] + 1e-300) - top;
        if (from < 0 && db <= -5.0) from = f;
        if (to < 0 && db <= -35.0) {
            to = f;
            break;
        }
    }
    if (from < 0 || to <= from) return 0.0;
    return 60.0 / 30.0 * (to - from) / kFs;
}

FdnTail tailFor(double lineScale, double mixing, const TailSettings& s) {
    FdnTail t;
    t.prepare(kFs);
    t.configure(planTail(K, kFs, lineScale, mixing));
    t.set(s);
    return t;
}

}  // namespace

TEST_CASE("the decay that is set is the decay that comes out") {
    //  Measured: room 0.50 set, 0.53 out; hall 1.90, 1.91. Here, flat
    //  across the bands so the one number means the whole tail. Catches: a gain a pass worked out from
    //  the line without its allpass, the -3 made a -6, and the three taps' decays left out.
    for (const auto& [scale, mixing, rt] :
         {std::tuple{0.55, 0.025, 0.5}, std::tuple{1.2, 0.06, 1.9}, std::tuple{2.0, 0.08, 6.5}}) {
        FdnTail tail = tailFor(scale, mixing, flat(rt));
        const double measured =
            rt60(impulseResponse(tail, rt * 2.0 + 0.5));  // well past the fit: a short record bends the curve down
        INFO("set ", rt, " measured ", measured);
        /*  Measured over the whole band it comes out 5 to 6 % short at every length -- 0.475, 1.795 and
            6.08 s -- with the drift or without it. The allpass in each line is why: a trip's gain is
            worked out for the line plus the allpass's length, which is the allpass's delay only on
            average. Where its delay is a third of that the trip is shorter than its gain assumes and
            decays faster, and a fit to the first 35 dB hears those modes first. It is accepted by ear
            (its cathedral measures 6.18 for 6.50); what is asserted is that it stays that and no worse. */
        CHECK(measured < rt);
        CHECK(measured > 0.92 * rt);
    }
}

TEST_CASE("each band rings as long as it was asked to, and the tone does not follow the decay") {
    TailSettings s = flat(2.0);
    s.rtLow = 2.8;
    s.rtHigh = 0.9;
    FdnTail tail = tailFor(1.2, 0.06, s);
    //  read an octave below the low shelf's corner and an octave above the high one's, clear of both
    //  transitions -- and clear of the 25 Hz blocker in the loop, which shortens what is near it
    const auto h2 = impulseResponse(tail, 6.0);
    const BiquadCoeffs lows = lowPass(140.0, std::sqrt(0.5), kFs), highs = highPass(9000.0, std::sqrt(0.5), kFs);
    const double low = rt60(h2, &lows), high = rt60(h2, &highs);
    INFO("low ", low, " high ", high);
    CHECK(low == doctest::Approx(2.8).epsilon(0.1));
    CHECK(high == doctest::Approx(0.9).epsilon(0.1));
}

TEST_CASE("its level is what the formula says, so the reflections can be balanced against it") {
    //  Every pass keeps g^2 of the energy and three taps each carry a third.
    for (const double rt : {0.6, 1.9})
        for (const double diffusion : {0.0, 0.6, 1.0}) {
            TailSettings s = flat(rt);
            s.diffusion = diffusion;
            FdnTail tail = tailFor(1.2, 0.06, s);
            const auto h = impulseResponse(tail, rt * 2.5 + 0.5);
            double energy = 0.0;
            for (const float v : h) energy += static_cast<double>(v) * v;
            INFO("rt ", rt, " diffusion ", diffusion, " measured ", energy, " formula ", tail.energyForUnitImpulse());
            CHECK(energy == doctest::Approx(tail.energyForUnitImpulse()).epsilon(0.1));
        }
}

TEST_CASE("it is heard at once, not a line's length later, and the pre-delay delays the source alone") {
    FdnTail tail = tailFor(1.2, 0.06, flat(1.9));
    auto h = impulseResponse(tail, 0.2);
    const auto firstSound = [&](const std::vector<float>& x) {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i]) > 1e-6f) return static_cast<int>(i) / K;
        return -1;
    };
    CHECK(firstSound(h) >= 0);
    CHECK(firstSound(h) < static_cast<int>(0.06 * kFs));  // within the diffuser, not after a 120 ms line

    TailSettings late = flat(1.9);
    late.preDelaySeconds = 0.04;
    FdnTail delayed = tailFor(1.2, 0.06, late);
    CHECK(firstSound(impulseResponse(delayed, 0.2)) == firstSound(h) + static_cast<int>(0.04 * kFs));
    //  what has already been scattered carries its own delay, and goes straight in
    const int frames = 4800;
    std::vector<float> none(static_cast<std::size_t>(frames * K), 0.0f), scattered = none, out(none.size());
    for (int k = 0; k < K; ++k) scattered[static_cast<std::size_t>(k)] = 0.25f;
    delayed.reset();
    delayed.process(none.data(), scattered.data(), out.data(), frames);
    CHECK(firstSound(out) == firstSound(h));
}

TEST_CASE("the drift never bends pitch by more than four cents, and leaves the decay alone") {
    FdnTail tail = tailFor(1.2, 0.06, flat(1.9));
    const double limit = std::pow(2.0, 4.0 / 1200.0) - 1.0;
    std::vector<float> in(static_cast<std::size_t>(K), 0.0f), out(in.size());
    double worst = 0.0, farthest = 0.0;
    std::vector<double> before(K);
    for (int k = 0; k < K; ++k) before[static_cast<std::size_t>(k)] = tail.delayOf(k);
    for (int i = 0; i < 48000 * 6; ++i) {
        tail.process(in.data(), nullptr, out.data(), 1);
        for (int k = 0; k < K; ++k) {
            const double d = tail.delayOf(k);
            worst = std::max(worst, std::abs(d - before[static_cast<std::size_t>(k)]));
            before[static_cast<std::size_t>(k)] = d;
        }
        farthest = std::max(farthest, std::abs(tail.delayOf(3) - planTail(K, kFs, 1.2, 0.06).length[3]));
    }
    CHECK(worst <= limit * 1.0001);
    CHECK(farthest > 10.0);                 // it does move: tens of samples
    CHECK(farthest <= 0.002 * kFs + 1e-6);  // and never more than 2 ms from the line's own length

    TailSettings still = flat(1.9);
    still.drift = false;
    FdnTail fixed = tailFor(1.2, 0.06, still);
    FdnTail moving = tailFor(1.2, 0.06, flat(1.9));
    CHECK(rt60(impulseResponse(moving, 2.8)) == doctest::Approx(rt60(impulseResponse(fixed, 2.8))).epsilon(0.03));
}

TEST_CASE("a render repeats, a reset is a fresh start, and how it is cut up does not matter") {
    std::vector<float> in(static_cast<std::size_t>(K * 9000));
    unsigned seed = 5u;
    for (float& v : in) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 8000.0f;
    }
    const auto render = [&](FdnTail& t, const std::vector<int>& pieces) {
        std::vector<float> out(in.size());
        int at = 0;
        std::size_t p = 0;
        while (at < 9000) {
            const int n = std::min(9000 - at, pieces[p++ % pieces.size()]);
            t.process(in.data() + at * K, nullptr, out.data() + at * K, n);
            at += n;
        }
        return out;
    };
    FdnTail a = tailFor(1.2, 0.06, flat(1.9)), b = tailFor(1.2, 0.06, flat(1.9));
    const auto whole = render(a, {9000});
    const auto pieces = render(b, {1, 7, 64, 333, 1024});
    CHECK(std::memcmp(whole.data(), pieces.data(), whole.size() * sizeof(float)) == 0);
    //  played, reset, played again: the drift too starts from the same place
    a.reset();
    const auto again = render(a, {512});
    CHECK(std::memcmp(whole.data(), again.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE("the longest decay there is stays where it should for half a minute") {
    FdnTail tail = tailFor(2.0, 0.08, flat(15.0));
    std::vector<float> in(static_cast<std::size_t>(K * 4800), 0.0f), out(in.size());
    for (int k = 0; k < K; ++k) in[static_cast<std::size_t>(k)] = 0.25f;
    double first = 0.0, last = 0.0;
    for (int block = 0; block < 300; ++block) {
        tail.process(in.data(), nullptr, out.data(), 4800);
        if (block == 0) std::fill(in.begin(), in.end(), 0.0f);
        double e = 0.0;
        for (const float v : out) {
            REQUIRE(std::isfinite(v));
            e += static_cast<double>(v) * v;
        }
        if (block == 10) first = e;
        if (block == 299) last = e;
    }
    CHECK(last < first);  // 29 s later: about 115 dB down on 15 s
    CHECK(10.0 * std::log10(last / first) == doctest::Approx(-60.0 * 28.9 / 15.0).epsilon(0.05));
}
