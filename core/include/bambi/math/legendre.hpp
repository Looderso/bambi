// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

namespace bambi {

/// Gauss-Legendre quadrature on [-1, 1]: n nodes, exact for polynomials of degree <= 2n - 1.
/// With equispaced azimuths it integrates products of spherical harmonics exactly.
void gaussLegendre(int n, std::vector<double>& nodes, std::vector<double>& weights);

/// Legendre polynomial P_n(x), by the standard three-term recurrence.
double legendreP(int n, double x);

/*  Associated Legendre P_n^m(x), m >= 0, without the Condon-Shortley phase, as AmbiX requires.
 *  Including the (-1)^m factor mirrors the field left-to-right while every magnitude, and so every
 *  normalisation test, stays intact; test_sh.cpp checks against an independent implementation.
 */
double legendreAssoc(int n, int m, double x);

/*  Per-order weights of a spherical cap of half-angle alpha (radians), order+1 values:
 *
 *      w_n = [P_{n-1}(cos a) - P_{n+1}(cos a)] / [(2n+1)(1 - cos a)],   w_0 = 1
 *
 *  alpha 0 gives all ones (no widening); alpha pi gives [1, 0, 0, ...] (pure omni).
 */
void capWeights(double alphaRad, int order, std::span<double> w);

/// max-rE decoder weighting, order+1 values.
void maxRE(int order, std::span<double> w);

}  // namespace bambi
