// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/math/legendre.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/vec3.hpp"

namespace bambi {

double legendreP(int n, double x) {
    if (n <= 0) return 1.0;
    if (n == 1) return x;
    double p0 = 1.0, p1 = x;
    for (int k = 1; k < n; ++k) {
        const double p2 = ((2.0 * k + 1.0) * x * p1 - k * p0) / (k + 1.0);
        p0 = p1;
        p1 = p2;
    }
    return p1;
}

void capWeights(double alphaRad, int order, std::span<double> w) {
    const double a = std::cos(clampd(alphaRad, 0.0, kPi));
    const double den = 1.0 - a;
    w[0] = 1.0;
    if (den < 1e-9) {
        // alpha -> 0 is the removable singularity; the limit is 1 for every order.
        for (int n = 1; n <= order; ++n) w[static_cast<std::size_t>(n)] = 1.0;
        return;
    }
    //  One recurrence carried from P_0 to P_{order+1}, instead of two fresh ones per order,
    //  which would cost O(order^2). Same operations in the same sequence as legendreP, so
    //  the weights are bit-identical to computing each one directly.
    double pPrev = 1.0, pCur = a;  // P_{n-1}, P_n, starting at n = 1
    for (int n = 1; n <= order; ++n) {
        const double pNext = ((2.0 * n + 1.0) * a * pCur - n * pPrev) / (n + 1.0);  // P_{n+1}
        w[static_cast<std::size_t>(n)] = (pPrev - pNext) / ((2.0 * n + 1.0) * den);
        pPrev = pCur;
        pCur = pNext;
    }
}

void maxRE(int order, std::span<double> w) {
    const double t = std::cos(137.9 * kDeg2Rad / (order + 1.51));
    for (int n = 0; n <= order; ++n) w[static_cast<std::size_t>(n)] = legendreP(n, t);
}

void gaussLegendre(int n, std::vector<double>& nodes, std::vector<double>& weights) {
    nodes.assign(static_cast<std::size_t>(std::max(n, 0)), 0.0);
    weights.assign(nodes.size(), 0.0);
    const auto legendreAndDerivative = [n](double z, double& p, double& dp) {
        double p1 = 1.0, p2 = 0.0;
        for (int j = 1; j <= n; ++j) {
            const double p3 = p2;
            p2 = p1;
            p1 = ((2.0 * j - 1.0) * z * p2 - (j - 1.0) * p3) / j;
        }
        p = p1;
        dp = n * (z * p1 - p2) / (z * z - 1.0);
    };
    for (int i = 0; i < n; ++i) {
        //  Newton from the classic asymptotic guess; the roots are simple and well separated,
        //  so a handful of steps reaches machine precision.
        double z = std::cos(kPi * (i + 0.75) / (n + 0.5));
        double p = 0.0, dp = 0.0;
        for (int it = 0; it < 100; ++it) {
            legendreAndDerivative(z, p, dp);
            const double step = p / dp;
            z -= step;
            if (std::abs(step) < 1e-16) break;
        }
        legendreAndDerivative(z, p, dp);  // the weight wants the derivative AT the root
        nodes[static_cast<std::size_t>(i)] = z;
        weights[static_cast<std::size_t>(i)] = 2.0 / ((1.0 - z * z) * dp * dp);
    }
}

void quadratureGrid(int rings, int azimuths, std::vector<Vec3>& directions, std::vector<double>& weights) {
    std::vector<double> z, wz;
    gaussLegendre(rings, z, wz);  // nodes in [-1, 1], weights summing to 2
    directions.clear();
    weights.clear();
    for (int i = 0; i < rings; ++i) {
        const double r =
            std::sqrt(std::max(0.0, 1.0 - z[static_cast<std::size_t>(i)] * z[static_cast<std::size_t>(i)]));
        for (int j = 0; j < azimuths; ++j) {
            const double phi = 2.0 * kPi * j / azimuths;
            directions.push_back({r * std::cos(phi), r * std::sin(phi), z[static_cast<std::size_t>(i)]});
            weights.push_back(wz[static_cast<std::size_t>(i)] / 2.0 / azimuths);
        }
    }
}

}  // namespace bambi
