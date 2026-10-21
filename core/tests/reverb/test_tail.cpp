// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
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
    //  Flat across the bands, so the one number means the whole tail. Catches: a gain a pass worked out from the
    //  line without its allpass, the -3 made a -6, and a read between samples that takes a share of the highs
    //  each pass and shortens the decay.
    for (const auto& [scale, mixing, rt] :
         {std::tuple{0.55, 0.025, 0.5}, std::tuple{1.2, 0.06, 1.9}, std::tuple{2.0, 0.08, 6.5}}) {
        FdnTail tail = tailFor(scale, mixing, flat(rt));
        const double measured =
            rt60(impulseResponse(tail, rt * 2.0 + 0.5));  // well past the fit: a short record bends the curve down
        INFO("set ", rt, " measured ", measured);
        CHECK(std::abs(measured / rt - 1.0) < 0.02);  // measured: 0.500, 1.902 and 6.50 s
    }
}

TEST_CASE("each band rings as long as it was asked to, and the tone does not follow the decay") {
    //  Catches: a band's shelf in the loop left out, a sub-sonic blocker that reaches up into the lows and shortens
    //  them, a read between samples that dulls the highs each pass, and the input's correction left out or worked
    //  out from the decay times rather than from the energy the lines hold.
    TailSettings s = flat(2.0);
    s.rtLow = 2.8;
    s.rtHigh = 0.9;
    FdnTail tail = tailFor(1.2, 0.06, s);
    //  read an octave below the low shelf's corner and an octave above the high one's, clear of both transitions
    const auto h2 = impulseResponse(tail, 6.0);
    const BiquadCoeffs lows = lowPass(140.0, std::sqrt(0.5), kFs), highs = highPass(9000.0, std::sqrt(0.5), kFs);
    const double low = rt60(h2, &lows), high = rt60(h2, &highs);
    INFO("low ", low, " high ", high);
    CHECK(std::abs(low / 2.8 - 1.0) < 0.03);  // measured 2.76 and 0.89 s
    CHECK(std::abs(high / 0.9 - 1.0) < 0.03);

    //  and each band is as loud as in a tail that rings evenly at the mid's decay: a band's energy grows with how
    //  long it rings, and the input's correction takes that back out (measured -0.04 dB in the lows, +0.01 in the
    //  highs; worked out from the decay times instead, -0.42 and +1.28)
    FdnTail even = tailFor(1.2, 0.06, flat(2.0));
    const auto e2 = impulseResponse(even, 6.0);
    const auto bandDb = [&](const BiquadCoeffs& band) {
        double shaped = 0.0, flatOnes = 0.0;
        for (int k = 0; k < K; ++k) {
            BiquadState a1, a2, b1, b2;
            for (std::size_t f = 0; f < h2.size() / K; ++f) {
                const double x = run(band, a2, run(band, a1, h2[f * K + static_cast<std::size_t>(k)]));
                const double y = run(band, b2, run(band, b1, e2[f * K + static_cast<std::size_t>(k)]));
                shaped += x * x;
                flatOnes += y * y;
            }
        }
        return 10.0 * std::log10(shaped / flatOnes);
    };
    INFO("low ", bandDb(lows), " dB, high ", bandDb(highs), " dB against an even tail");
    CHECK(std::abs(bandDb(lows)) < 0.25);
    CHECK(std::abs(bandDb(highs)) < 0.25);
}

TEST_CASE("its level is what the formula says, so the reflections can be balanced against it") {
    //  Catches: the diffuser's share left out of the formula, the two taps along each line counted at the wrong
    //  level, and a decay shorter than the gains a pass say. Every pass keeps g^2 of the energy and three taps each
    //  carry a third; the formula is for what reaches the lines, so the input's cuts take their own share first.
    const auto cutsEnergy = [](const TailSettings& s) {
        const BiquadCoeffs low = highPass(s.lowCutHz, std::sqrt(0.5), kFs),
                           high = lowPass(s.highCutHz, std::sqrt(0.5), kFs);
        BiquadState a, b;
        double e = 0.0;
        for (int i = 0; i < static_cast<int>(kFs); ++i) {
            const double v = run(high, b, run(low, a, i == 0 ? 1.0 : 0.0));
            e += v * v;
        }
        return e;
    };
    for (const double rt : {0.6, 1.9})
        for (const double diffusion : {0.0, 0.6, 1.0}) {
            TailSettings s = flat(rt);
            s.diffusion = diffusion;
            FdnTail tail = tailFor(1.2, 0.06, s);
            const auto h = impulseResponse(tail, rt * 2.5 + 0.5);
            double energy = 0.0;
            for (const float v : h) energy += static_cast<double>(v) * v;
            INFO("rt ", rt, " diffusion ", diffusion, " measured ", energy, " formula ", tail.energyForUnitImpulse());
            CHECK(std::abs(energy / (tail.energyForUnitImpulse() * cutsEnergy(s)) - 1.0) <
                  0.02);  // measured within 0.7 %
        }
}

TEST_CASE("it is heard at once, not a line's length later, and the pre-delay delays the source alone") {
    //  Catches: the pre-delay ignored, what was already scattered delayed by it too, and a line heard only from a third
    //  of the way along.
    FdnTail tail = tailFor(1.2, 0.06, flat(1.9));
    auto h = impulseResponse(tail, 0.2);
    const auto firstSound = [&](const std::vector<float>& x) {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i]) > 1e-6f) return static_cast<int>(i) / K;
        return -1;
    };
    CHECK(firstSound(h) >= 0);
    CHECK(firstSound(h) < static_cast<int>(0.005 * kFs));  // where a line starts, not a third of a line later

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
    //  Catches: a drift that changes the delay faster than four cents allow, and one that swings farther than 2 ms.
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
    //  Catches: the drift's random state not restarted by a reset.
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
    //  Catches: an allpass in the loop that is not lossless, which shows only far down a long decay.
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

TEST_CASE("nothing in the loop gains: a tail asked to ring for ever still fades, in the highs too") {
    //  Catches: a sub-sonic blocker not normalised, which lifts the highs a little every pass, so a decay long
    //  enough grows there instead of falling.
    TailSettings s = flat(1.0e6);  // past what the gains allow: each pass held to its most, 0.0004 dB down
    s.drift = false;
    FdnTail tail = tailFor(1.2, 0.06, s);
    tail.reset();
    const int frames = static_cast<int>(7 * kFs);
    std::vector<float> in(static_cast<std::size_t>(frames * K), 0.0f), out(in.size());
    unsigned seed = 9u;
    for (int f = 0; f < static_cast<int>(0.2 * kFs); ++f)
        for (int k = 0; k < K; ++k) {
            seed = seed * 1664525u + 1013904223u;
            in[static_cast<std::size_t>(f * K + k)] =
                static_cast<float>(static_cast<int>(seed >> 8) % 2001 - 1000) / 4000.0f;
        }
    tail.process(in.data(), nullptr, out.data(), frames);
    const BiquadCoeffs highs = highPass(12000.0, std::sqrt(0.5), kFs);
    const auto highEnergy = [&](int fromSecond) {
        double e = 0.0;
        for (int k = 0; k < K; ++k) {
            BiquadState a, b;
            for (int f = 0; f < frames; ++f) {
                const double v = run(highs, b, run(highs, a, out[static_cast<std::size_t>(f * K + k)]));
                if (f >= fromSecond * static_cast<int>(kFs) && f < (fromSecond + 1) * static_cast<int>(kFs)) e += v * v;
            }
        }
        return e;
    };
    const double early = highEnergy(1), late = highEnergy(6);
    INFO("highs from the 2nd to the 7th second: ", 10.0 * std::log10(late / early), " dB");
    CHECK(late < early);                            // about 0.01 dB over these five seconds
    CHECK(10.0 * std::log10(late / early) > -0.1);  // and no more than that
}
