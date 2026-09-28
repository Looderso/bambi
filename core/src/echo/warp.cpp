// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/warp.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {
namespace {

//  At azimuth zero each +m channel is its Legendre term alone, so the terms come from the encoder's
//  own harmonics and share their normalisation and signs.
void termsAt(double z, int order, std::span<double> out) {
    const double zz = clampd(z, -1.0, 1.0);
    shSN3D(Vec3{std::sqrt(std::max(0.0, 1.0 - zz * zz)), 0.0, zz}, order, out);
}

}  // namespace

void WarpBuilder::prepare(int order, int nodes) {
    order_ = order;
    gaussLegendre(nodes, z_, weight_);
    const auto channels = static_cast<std::size_t>(numChannels(order));
    atNode_.assign(z_.size() * channels, 0.0);
    atImage_.assign(channels, 0.0);
    for (std::size_t k = 0; k < z_.size(); ++k)
        termsAt(z_[k], order, std::span<double>(atNode_).subspan(k * channels, channels));
}

void WarpBuilder::build(double skew, std::span<double> blocks) noexcept {
    const double a = clampd(skew, -kMaxSkew, kMaxSkew);
    const int N = order_;
    const auto channels = static_cast<std::size_t>(numChannels(N));
    std::fill(blocks.begin(), blocks.begin() + warpSize(N), 0.0);

    for (std::size_t k = 0; k < z_.size(); ++k) {
        termsAt(slideZ(z_[k], a), N, atImage_);
        const double* node = atNode_.data() + k * channels;
        int at = 0;
        for (int m = 0; m <= N; ++m) {
            const int w = N - m + 1;
            for (int i = 0; i < w; ++i) {
                const int ni = m + i;
                const double image = weight_[k] * atImage_[static_cast<std::size_t>(ni * ni + ni + m)];
                for (int j = 0; j < w; ++j) {
                    const int nj = m + j;
                    blocks[static_cast<std::size_t>(at + w * i + j)] += image * node[nj * nj + nj + m];
                }
            }
            at += w * w;
        }
    }

    //  Azimuth, in closed form: 2 pi for m = 0 and pi otherwise, over 4 pi -- times the (2n+1) of
    //  the column.
    int at = 0;
    for (int m = 0; m <= N; ++m) {
        const int w = N - m + 1;
        for (int i = 0; i < w; ++i)
            for (int j = 0; j < w; ++j)
                blocks[static_cast<std::size_t>(at + w * i + j)] *= (2.0 * (m + j) + 1.0) * (m == 0 ? 0.5 : 0.25);
        at += w * w;
    }
}

void applyWarp(std::span<const double> blocks, int order, std::span<const double> in, std::span<double> out) noexcept {
    int at = 0;
    for (int m = 0; m <= order; ++m) {
        const int w = order - m + 1;
        for (const int sign : {1, -1}) {
            if (m == 0 && sign < 0) break;
            for (int i = 0; i < w; ++i) {
                const int ni = m + i;
                double sum = 0.0;
                for (int j = 0; j < w; ++j) {
                    const int nj = m + j;
                    sum += blocks[static_cast<std::size_t>(at + w * i + j)] *
                           in[static_cast<std::size_t>(nj * nj + nj + sign * m)];
                }
                out[static_cast<std::size_t>(ni * ni + ni + sign * m)] = sum;
            }
        }
        at += w * w;
    }
}

}  // namespace bambi
