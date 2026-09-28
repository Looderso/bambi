// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/tailplan.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {
namespace {

constexpr double kLineMs[2] = {100.0, 200.0};
constexpr double kAllpassMs[2] = {5.0, 15.0};
constexpr double kDiffuserMs[kDiffuserSteps] = {20.0, 40.0, 80.0, 160.0};  // proportions; they add up to 300
constexpr double kMaxLineScale = 2.0, kMaxMixing = 0.080;

bool isPrime(int n) {
    if (n < 2) return false;
    for (int i = 2; static_cast<long long>(i) * i <= n; ++i)
        if (n % i == 0) return false;
    return true;
}

//  Lehmer's generator, written out so that the network is the same on every machine and in every build.
struct Seeded {
    long long state{4242};
    double next() {
        state = state * 16807 % 2147483647;
        return static_cast<double>(state) / 2147483647.0;
    }
};

void shuffle(std::array<int, kMaxTailLines>& a, int n, Seeded& rnd) {
    for (int i = 0; i < n; ++i) a[static_cast<std::size_t>(i)] = i;
    for (int i = n - 1; i > 0; --i) {
        const int j = static_cast<int>(std::floor(rnd.next() * (i + 1)));
        std::swap(a[static_cast<std::size_t>(i)], a[static_cast<std::size_t>(j)]);
    }
}

//  n primes spread geometrically from a to b samples, none used twice.
void primesBetween(double a, double b, int n, int least, std::array<int, kMaxTailLines>& out) {
    for (int i = 0; i < n; ++i) {
        int L = std::max(
            least, static_cast<int>(std::lround(a * std::pow(b / a, static_cast<double>(i) / std::max(1, n - 1)))));
        const auto used = [&](int v) { return std::find(out.begin(), out.begin() + i, v) != out.begin() + i; };
        while (!isPrime(L) || used(L)) ++L;
        out[static_cast<std::size_t>(i)] = L;
    }
}

}  // namespace

double TailPlan::totalSeconds(double sampleRate) const {
    long long sum = 0;
    for (int i = 0; i < lines; ++i) sum += length[static_cast<std::size_t>(i)];
    return static_cast<double>(sum) / sampleRate;
}

TailPlan planTail(int lines, double sampleRate, double lineScale, double mixingTime) {
    TailPlan p;
    int K = 2;
    while (K * 2 <= std::clamp(lines, 2, kMaxTailLines)) K *= 2;
    p.lines = K;
    const double scale = std::clamp(lineScale, 0.1, kMaxLineScale), mixing = std::clamp(mixingTime, 0.001, kMaxMixing);
    Seeded rnd;

    //  The diffuser first, then the allpasses, then the output: the order the numbers are drawn in is
    //  part of what the network is.
    const double diffuserScale = mixing * 1000.0 / 300.0;
    std::array<int, kMaxTailLines> order{}, drawn{};
    for (int s = 0; s < kDiffuserSteps; ++s) {
        const double range = kDiffuserMs[s] * diffuserScale / 1000.0 * sampleRate;
        //  one delay in each K-th of the step's range, so they are spread and not bunched
        for (int k = 0; k < K; ++k)
            drawn[static_cast<std::size_t>(k)] =
                std::max(1, static_cast<int>(std::lround(range * (k + rnd.next()) / K)));
        shuffle(order, K, rnd);
        for (int k = 0; k < K; ++k)
            p.diffuserDelay[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] =
                drawn[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
    }

    std::array<int, kMaxTailLines> sorted{};
    primesBetween(kAllpassMs[0] * scale / 1000.0 * sampleRate, kAllpassMs[1] * scale / 1000.0 * sampleRate, K, 3,
                  sorted);
    shuffle(order, K, rnd);
    for (int k = 0; k < K; ++k)
        p.allpass[static_cast<std::size_t>(k)] = sorted[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];

    for (int s = 0; s < kDiffuserSteps; ++s) shuffle(p.diffuserFrom[static_cast<std::size_t>(s)], K, rnd);
    for (int s = 0; s < kDiffuserSteps; ++s)
        for (int k = 0; k < K; ++k)
            p.diffuserSign[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] = rnd.next() < 0.5 ? -1.0f : 1.0f;
    shuffle(p.outFrom, K, rnd);
    for (int k = 0; k < K; ++k) p.outSign[static_cast<std::size_t>(k)] = rnd.next() < 0.5 ? -1.0f : 1.0f;

    //  The lines take nothing from the seed: primes from 100 to 200 ms, then long and short interleaved
    //  so that neighbours in the Hadamard's pairs are not alike.
    primesBetween(kLineMs[0] * scale / 1000.0 * sampleRate, kLineMs[1] * scale / 1000.0 * sampleRate, K, 2, sorted);
    for (int k = 0; k < K; ++k) p.length[static_cast<std::size_t>(k)] = sorted[static_cast<std::size_t>((k * 7) % K)];
    return p;
}

TailLimits tailLimits(double sampleRate) {
    //  past the longest a plan can ask for: a prime search only ever goes up, and not far
    const auto above = [&](double ms) { return static_cast<int>(std::ceil(ms / 1000.0 * sampleRate * 1.02)) + 64; };
    return {above(kLineMs[1] * kMaxLineScale), above(kAllpassMs[1] * kMaxLineScale),
            above(kDiffuserMs[kDiffuserSteps - 1] * kMaxMixing * 1000.0 / 300.0)};
}

}  // namespace bambi
