// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/math/sh.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {
void shSN3D(Vec3 p, int order, std::span<double> out) {
    const double z = clampd(p.z, -1.0, 1.0);  // = cos(polar angle), since z is up
    const double phi = std::atan2(p.y, p.x);
    const double s = std::sqrt(std::max(0.0, 1.0 - z * z));

    /*  Iterated by DEGREE outer, order inner, so each associated Legendre recurrence runs
     *  once and is then carried upward. The obvious nesting — order outer, calling a
     *  self-contained legendreAssoc per (n, m) — restarts that recurrence from scratch
     *  every time and is O(order^3) where O(order^2) is available.
     *
     *  The azimuth terms use the Chebyshev recurrence for the same reason: two trig calls
     *  in total instead of one per channel (64 of them at order 7).
     */
    const double c1 = std::cos(phi), s1 = std::sin(phi);
    double cosPrev = 1.0, cosCur = c1;  // cos(0.phi), cos(1.phi)
    double sinPrev = 0.0, sinCur = s1;

    double pmm = 1.0;      // P_m^m
    double normPmm = 1.0;  // SN3D scale for (m, m), carried by its own recurrence

    for (int m = 0; m <= order; ++m) {
        if (m > 0) {
            // P_m^m = (2m-1)!! * (1-z^2)^(m/2). No (-1)^m: AmbiX omits Condon-Shortley.
            pmm *= (2.0 * m - 1.0) * s;
            // N_m^m = sqrt(2 / (2m)!) built incrementally from N_{m-1}^{m-1}.
            normPmm /= std::sqrt((2.0 * m - 1.0) * (2.0 * m));
            if (m == 1) normPmm *= std::sqrt(2.0);
        }

        const double cm = m == 0 ? 1.0 : (m == 1 ? c1 : cosCur);
        const double sm = m == 0 ? 0.0 : (m == 1 ? s1 : sinCur);

        double prev2 = 0.0, prev1 = pmm, norm = normPmm;
        for (int n = m; n <= order; ++n) {
            double leg;
            if (n == m) {
                leg = pmm;
            } else if (n == m + 1) {
                leg = z * (2.0 * m + 1.0) * pmm;
                prev2 = prev1;
                prev1 = leg;
            } else {
                leg = (z * (2.0 * n - 1.0) * prev1 - (n + m - 1.0) * prev2) / (n - m);
                prev2 = prev1;
                prev1 = leg;
            }
            if (n > m) {
                // N_n^m / N_{n-1}^m = sqrt( (n-m) / (n+m) )
                norm *= std::sqrt(static_cast<double>(n - m) / static_cast<double>(n + m));
            }
            const double base = norm * leg;
            out[static_cast<std::size_t>(n * n + n + m)] = base * cm;
            if (m > 0) out[static_cast<std::size_t>(n * n + n - m)] = base * sm;
        }

        if (m >= 1) {  // cos((m+1)phi) = 2 cos(phi) cos(m phi) - cos((m-1)phi)
            const double nc = 2.0 * c1 * cosCur - cosPrev;
            const double ns = 2.0 * c1 * sinCur - sinPrev;
            cosPrev = cosCur;
            cosCur = nc;
            sinPrev = sinCur;
            sinCur = ns;
        }
    }
}

void shSN3D_ref(Vec3 p, int order, std::span<double> out) {
    const double x = p.x, y = p.y, z = p.z;
    out[0] = 1.0;
    if (order >= 1) {
        out[1] = y;
        out[2] = z;
        out[3] = x;
    }
    if (order >= 2) {
        const double s3 = std::sqrt(3.0);
        out[4] = s3 * x * y;
        out[5] = s3 * y * z;
        out[6] = (3.0 * z * z - 1.0) / 2.0;
        out[7] = s3 * x * z;
        out[8] = s3 / 2.0 * (x * x - y * y);
    }
    if (order >= 3) {
        const double a = std::sqrt(5.0 / 8.0), b = std::sqrt(15.0), c = std::sqrt(3.0 / 8.0);
        out[9] = a * y * (3.0 * x * x - y * y);
        out[10] = b * x * y * z;
        out[11] = c * y * (5.0 * z * z - 1.0);
        out[12] = z * (5.0 * z * z - 3.0) / 2.0;
        out[13] = c * x * (5.0 * z * z - 1.0);
        out[14] = b / 2.0 * z * (x * x - y * y);
        out[15] = a * x * (x * x - 3.0 * y * y);
    }
}

}  // namespace bambi
