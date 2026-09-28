// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/math/shrotation.hpp"

#include <cmath>

namespace bambi {

Vec3 apply(const Mat3& r, Vec3 v) {
    return {r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
            r[6] * v.x + r[7] * v.y + r[8] * v.z};
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
                sum += a[static_cast<std::size_t>(3 * i + k)] * b[static_cast<std::size_t>(3 * k + j)];
            out[static_cast<std::size_t>(3 * i + j)] = sum;
        }
    return out;
}

Mat3 transposed(const Mat3& r) { return {r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8]}; }

Mat3 rotationAbout(Vec3 axis, double angleRad) {
    const Vec3 k = unit(axis);
    const double c = std::cos(angleRad), s = std::sin(angleRad), t = 1.0 - c;
    return {t * k.x * k.x + c,       t * k.x * k.y - s * k.z, t * k.x * k.z + s * k.y,
            t * k.x * k.y + s * k.z, t * k.y * k.y + c,       t * k.y * k.z - s * k.x,
            t * k.x * k.z - s * k.y, t * k.y * k.z + s * k.x, t * k.z * k.z + c};
}

Mat3 axisToPole(Vec3 axis) {
    const Vec3 a = unit(axis), pole{0.0, 0.0, 1.0};
    const Vec3 about = cross(a, pole);
    const double s = length(about);
    if (s < 1e-12) return a.z > 0.0 ? kIdentity3 : rotationAbout({1.0, 0.0, 0.0}, kPi);
    return rotationAbout(about * (1.0 / s), std::atan2(s, a.z));
}

namespace {

/*  One order's block, read with m and n running -l..l. `lower` is the block of the order below and
 *  `first` the first-order block; the three helpers are the paper's P, and U, V, W built from it. */
struct Below {
    const double* first;  // 3 x 3
    const double* lower;  // (2l-1) x (2l-1)
    int l;

    double r1(int i, int j) const { return first[3 * (i + 1) + (j + 1)]; }
    double low(int a, int b) const {
        const int w = 2 * l - 1;
        return lower[w * (a + l - 1) + (b + l - 1)];
    }
    double P(int i, int a, int b) const {
        if (b == -l) return r1(i, 1) * low(a, -l + 1) + r1(i, -1) * low(a, l - 1);
        if (b == l) return r1(i, 1) * low(a, l - 1) - r1(i, -1) * low(a, -l + 1);
        return r1(i, 0) * low(a, b);
    }
    double U(int m, int n) const { return P(0, m, n); }
    double V(int m, int n) const {
        if (m == 0) return P(1, 1, n) + P(-1, -1, n);
        if (m > 0) {
            const double d = m == 1 ? 1.0 : 0.0;
            return P(1, m - 1, n) * std::sqrt(1.0 + d) - P(-1, -m + 1, n) * (1.0 - d);
        }
        const double d = m == -1 ? 1.0 : 0.0;
        return P(1, m + 1, n) * (1.0 - d) + P(-1, -m - 1, n) * std::sqrt(1.0 + d);
    }
    double W(int m, int n) const {
        if (m > 0) return P(1, m + 1, n) + P(-1, -m - 1, n);
        return P(1, m - 1, n) - P(-1, -m + 1, n);
    }
};

}  // namespace

void shRotation(const Mat3& r, int order, std::span<double> blocks) noexcept {
    if (order < 0 || static_cast<int>(blocks.size()) < rotationSize(order)) return;
    blocks[0] = 1.0;
    if (order == 0) return;

    //  First order: the rotation itself, with rows and columns in channel order -- y, z, x.
    double* first = blocks.data() + rotationOffset(1);
    constexpr int axisOf[3] = {1, 2, 0};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) first[3 * i + j] = r[static_cast<std::size_t>(3 * axisOf[i] + axisOf[j])];

    for (int l = 2; l <= order; ++l) {
        const Below below{first, blocks.data() + rotationOffset(l - 1), l};
        double* block = blocks.data() + rotationOffset(l);
        const int width = 2 * l + 1;
        for (int m = -l; m <= l; ++m)
            for (int n = -l; n <= l; ++n) {
                const double d = m == 0 ? 1.0 : 0.0;
                const double denom = std::abs(n) == l ? 2.0 * l * (2.0 * l - 1.0) : static_cast<double>(l * l - n * n);
                const int am = std::abs(m);
                const double u = std::sqrt((l * l - m * m) / denom);
                const double v = std::sqrt((1.0 + d) * (l + am - 1.0) * (l + am) / denom) * (1.0 - 2.0 * d) * 0.5;
                const double w = std::sqrt((l - am - 1.0) * (l - am) / denom) * (1.0 - d) * -0.5;
                double sum = 0.0;
                //  A term whose coefficient is zero is skipped, not multiplied out: its helper may
                //  reach outside the block below.
                if (u != 0.0) sum += u * below.U(m, n);
                if (v != 0.0) sum += v * below.V(m, n);
                if (w != 0.0) sum += w * below.W(m, n);
                block[width * (m + l) + (n + l)] = sum;
            }
    }
}

void rotateSH(std::span<const double> blocks, int order, std::span<const double> in, std::span<double> out) noexcept {
    for (int n = 0; n <= order; ++n) {
        const int width = 2 * n + 1, first = n * n;
        const double* block = blocks.data() + rotationOffset(n);
        for (int i = 0; i < width; ++i) {
            double sum = 0.0;
            for (int j = 0; j < width; ++j) sum += block[width * i + j] * in[static_cast<std::size_t>(first + j)];
            out[static_cast<std::size_t>(first + i)] = sum;
        }
    }
}

}  // namespace bambi
