// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-orders -- how high an ambisonic order the core can carry, and at what cost.
//
//  Either of two things could be the ceiling, so both are measured per order: whether the
//  encoding is still exact (SN3D orthonormality under exact quadrature), and what an encoder
//  instance costs. Format and host ceilings are printed alongside, because in practice those
//  are lower than either.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "bambi/encode/encoder.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"

using namespace bambi;
using Clock = std::chrono::steady_clock;

namespace {

template <typename F>
double medianMicros(int batches, int iters, F&& f) {
    std::vector<double> t;
    for (int b = 0; b < batches; ++b) {
        const auto t0 = Clock::now();
        for (int i = 0; i < iters; ++i) f();
        t.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / iters);
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

/*  Worst deviation from SN3D orthonormality: mean(Y_a Y_b) = delta_ab / (2n+1), integrated
 *  exactly. Every channel's normalisation, plus every pair up to 256 channels and a random
 *  sample of pairs above that (a full Gram matrix at 1296 channels is 1.7 million pairs). */
double orthonormalityError(int order) {
    const int ch = numChannels(order);
    std::vector<double> z, wz;
    gaussLegendre(order + 1, z, wz);
    const int nPhi = 2 * order + 2;
    std::vector<double> w, Y;  // Y is points x channels
    std::vector<double> y(static_cast<std::size_t>(ch));
    for (std::size_t i = 0; i < z.size(); ++i) {
        const double r = std::sqrt(std::max(0.0, 1.0 - z[i] * z[i]));
        for (int j = 0; j < nPhi; ++j) {
            const double phi = 2.0 * kPi * j / nPhi;
            shSN3D({r * std::cos(phi), r * std::sin(phi), z[i]}, order, y);
            Y.insert(Y.end(), y.begin(), y.end());
            w.push_back(wz[i] / (2.0 * nPhi));
        }
    }
    const std::size_t P = w.size();
    const auto inner = [&](int a, int b) {
        double s = 0.0;
        for (std::size_t p = 0; p < P; ++p)
            s += w[p] * Y[p * static_cast<std::size_t>(ch) + static_cast<std::size_t>(a)] *
                 Y[p * static_cast<std::size_t>(ch) + static_cast<std::size_t>(b)];
        return s;
    };
    double worst = 0.0;
    const auto check = [&](int a, int b) {
        const double want = a == b ? 1.0 / (2.0 * acnOrder(a) + 1.0) : 0.0;
        worst = std::max(worst, std::abs(inner(a, b) - want) * (2.0 * acnOrder(a) + 1.0));
    };
    if (ch <= 256) {
        for (int a = 0; a < ch; ++a)
            for (int b = a; b < ch; ++b) check(a, b);
    } else {
        for (int a = 0; a < ch; ++a) check(a, a);
        std::mt19937 rng(static_cast<unsigned>(order));
        std::uniform_int_distribution<int> pick(0, ch - 1);
        for (int k = 0; k < 4000; ++k) {
            //  Half the sample shares |m|: those pairs are the ones that rely on the z
            //  quadrature rather than cancelling in azimuth, so they are the hard ones.
            const int a = pick(rng);
            int b = pick(rng);
            if (k % 2 == 0) {
                const int m = acnDegree(a);
                const int n = std::abs(m) + static_cast<int>(rng() % static_cast<unsigned>(order - std::abs(m) + 1));
                b = n * n + n + m;
            }
            check(a, b);
        }
    }
    return worst;
}

const char* ceilingNote(int order) {
    const int ch = numChannels(order);
    if (order == 7) return "VST3 ceiling (64-bit speaker mask)";
    if (order == 10) return "REAPER track ceiling (128 ch)";
    if (order == 11) return "beyond a REAPER track";
    (void)ch;
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    int maxOrder = kMaxOrder;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--max") == 0) maxOrder = std::atoi(argv[i + 1]);
    maxOrder = std::clamp(maxOrder, 1, kMaxOrder);

#ifndef NDEBUG
    std::printf("\n  *** DEBUG BUILD -- these numbers are meaningless. ***\n");
#endif
    constexpr int kBlock = 128;
    const double blockUs = kBlock / 48000.0 * 1e6;

    std::printf("\nbambi-orders   (Release, 128-sample blocks at 48 kHz = %.3f ms)\n\n", blockUs / 1000.0);
    std::printf("  order    ch   orthonormal err   SH eval    encode block   of a core   32 instances\n");
    std::printf("  -----  ----   ---------------   --------   ------------   ---------   ------------\n");

    std::vector<float> in(kBlock);
    for (int i = 0; i < kBlock; ++i) in[static_cast<std::size_t>(i)] = 0.5f * std::sin(0.07f * static_cast<float>(i));

    for (int order = 1; order <= maxOrder; ++order) {
        const int ch = numChannels(order);
        const double err = orthonormalityError(order);

        std::vector<double> y(static_cast<std::size_t>(ch));
        double a = 0.0;
        const int shIters = std::max(200, 400000 / ch);
        const double shUs = medianMicros(9, shIters, [&] {
            a += 0.001;
            shSN3D(fromAzEl(a, 0.3), order, y);
        });

        Encoder enc;
        enc.prepare(order);
        std::vector<std::vector<float>> bufs(static_cast<std::size_t>(ch), std::vector<float>(kBlock, 0.0f));
        std::vector<float*> ptrs;
        for (auto& b : bufs) ptrs.push_back(b.data());
        double az = 0.0;
        const int encIters = std::max(50, 200000 / ch);
        const double encUs = medianMicros(9, encIters, [&] {
            az += 0.01;
            enc.setTarget(fromAzEl(az, 0.2), 0.8);
            enc.process(in.data(), ptrs, kBlock);
        });

        const double share = encUs / blockUs * 100.0;
        std::printf("  %5d  %4d   %15.1e   %6.3f us   %9.3f us   %8.3f%%   %10.2f%%   %s\n", order, ch, err, shUs,
                    encUs, share, share * 32.0, ceilingNote(order));
    }
    std::printf("\n  orthonormal err: worst |mean(Y_a Y_b) - delta_ab/(2n+1)| x (2n+1), exact quadrature\n\n");
    return 0;
}
