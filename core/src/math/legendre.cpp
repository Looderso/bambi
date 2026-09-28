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

double legendreAssoc(int n, int m, double x) {
    if (m < 0 || m > n) return 0.0;
    if (m == 0) return legendreP(n, x);

    // P_m^m = (2m-1)!! * (1-x^2)^(m/2).  No (-1)^m: AmbiX omits Condon-Shortley.
    const double s = std::sqrt(std::max(0.0, 1.0 - x * x));
    double pmm = 1.0;
    double fact = 1.0;
    for (int i = 1; i <= m; ++i) {
        pmm *= fact * s;
        fact += 2.0;
    }
    if (n == m) return pmm;

    // P_{m+1}^m = x (2m+1) P_m^m
    double pmmp1 = x * (2.0 * m + 1.0) * pmm;
    if (n == m + 1) return pmmp1;

    // P_k^m = [ x(2k-1) P_{k-1}^m - (k+m-1) P_{k-2}^m ] / (k-m)
    double prev = pmm, cur = pmmp1, out = 0.0;
    for (int k = m + 2; k <= n; ++k) {
        out = (x * (2.0 * k - 1.0) * cur - (k + m - 1.0) * prev) / (k - m);
        prev = cur;
        cur = out;
    }
    return out;
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

}  // namespace bambi
