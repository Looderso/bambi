// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

/*  Dense least squares over a spherical-harmonic basis: the weighted Gram matrix and the inverse of a
 *  symmetric positive-definite matrix through its Cholesky factor. Row-major, doubles, sizes in the
 *  hundreds: what a filter design and a head fit need, and nothing a real-time path calls.
 */
namespace bambi {

/// Replaces the symmetric positive-definite n x n matrix `g` (row-major) by its inverse.
inline void invertSpd(std::vector<double>& g, int n) {
    const auto sz = [](int v) { return static_cast<std::size_t>(v); };
    std::vector<double> l(g.size(), 0.0);  // lower triangular, L L^T = G
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = g[sz(i) * sz(n) + sz(j)];
            for (int k = 0; k < j; ++k) s -= l[sz(i) * sz(n) + sz(k)] * l[sz(j) * sz(n) + sz(k)];
            l[sz(i) * sz(n) + sz(j)] = i == j ? std::sqrt(std::max(s, 1e-300)) : s / l[sz(j) * sz(n) + sz(j)];
        }
    }
    //  G^-1 = L^-T L^-1: solve L X = I column by column, then L^T G^-1 = X.
    std::vector<double> x(g.size(), 0.0);
    for (int col = 0; col < n; ++col) {
        for (int i = 0; i < n; ++i) {
            double s = i == col ? 1.0 : 0.0;
            for (int k = 0; k < i; ++k) s -= l[sz(i) * sz(n) + sz(k)] * x[sz(k) * sz(n) + sz(col)];
            x[sz(i) * sz(n) + sz(col)] = s / l[sz(i) * sz(n) + sz(i)];
        }
        for (int i = n - 1; i >= 0; --i) {
            double s = x[sz(i) * sz(n) + sz(col)];
            for (int k = i + 1; k < n; ++k) s -= l[sz(k) * sz(n) + sz(i)] * g[sz(k) * sz(n) + sz(col)];
            g[sz(i) * sz(n) + sz(col)] = s / l[sz(i) * sz(n) + sz(i)];
        }
    }
}

/// The weighted Gram matrix Y^T W Y, channels x channels, of a basis stored directions x channels.
inline void gramMatrix(std::span<const double> basis, std::span<const double> weights, int channels,
                       std::vector<double>& out) {
    const auto sz = [](int v) { return static_cast<std::size_t>(v); };
    const int directions = static_cast<int>(weights.size());
    out.assign(sz(channels) * sz(channels), 0.0);
    for (int d = 0; d < directions; ++d) {
        const double* y = basis.data() + sz(d) * sz(channels);
        const double w = weights[sz(d)];
        for (int i = 0; i < channels; ++i)
            for (int j = 0; j < channels; ++j) out[sz(i) * sz(channels) + sz(j)] += w * y[i] * y[j];
    }
}

}  // namespace bambi
