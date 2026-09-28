// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/field.hpp"

#include <cstddef>

#include "bambi/math/sh.hpp"
#include "bambi/math/shrotation.hpp"

namespace bambi {
namespace {

//  out = M * in for one block, M column-major. A column at a time, so each inner loop is one
//  contiguous multiply-add the width of the block.
template <int W>
inline void applyBlock(const float* m, const float* in, float* out) {
    //  Summed in a local the width of the block and stored once: written straight to `out`, every
    //  column would be a store and a reload.
    float acc[W];
    for (int i = 0; i < W; ++i) acc[i] = m[i] * in[0];
    for (int j = 1; j < W; ++j) {
        const float x = in[j];
        const float* col = m + j * W;
        for (int i = 0; i < W; ++i) acc[i] += col[i] * x;
    }
    for (int i = 0; i < W; ++i) out[i] = acc[i];
}

inline void applyBlockAnyWidth(int w, const float* m, const float* in, float* out) {
    for (int i = 0; i < w; ++i) out[i] = m[i] * in[0];
    for (int j = 1; j < w; ++j) {
        const float x = in[j];
        const float* col = m + j * w;
        for (int i = 0; i < w; ++i) out[i] += col[i] * x;
    }
}

//  One frame, orders 0..N, every width a constant.
template <int N>
inline void rotateFrame(const float* blocks, const float* in, float* out) {
    if constexpr (N > 0) rotateFrame<N - 1>(blocks, in, out);
    applyBlock<2 * N + 1>(blocks + rotationOffset(N), in + N * N, out + N * N);
}

template <int N>
void rotateFrames(const float* blocks, const float* in, float* out, int frames) {
    constexpr int channels = (N + 1) * (N + 1);
    for (int f = 0; f < frames; ++f) rotateFrame<N>(blocks, in + f * channels, out + f * channels);
}

}  // namespace

void fieldBlocks(std::span<const double> blocks, int order, std::span<float> forward,
                 std::span<float> inverse) noexcept {
    for (int n = 0; n <= order; ++n) {
        const int w = 2 * n + 1, at = rotationOffset(n);
        for (int i = 0; i < w; ++i)
            for (int j = 0; j < w; ++j) {
                const auto v = static_cast<float>(blocks[static_cast<std::size_t>(at + w * i + j)]);  // row i, column j
                forward[static_cast<std::size_t>(at + w * j + i)] = v;
                inverse[static_cast<std::size_t>(at + w * i + j)] = v;
            }
    }
}

void rotateField(std::span<const float> blocks, int order, const float* in, float* out, int frames) noexcept {
    const float* m = blocks.data();
    switch (order) {
        case 0: rotateFrames<0>(m, in, out, frames); return;
        case 1: rotateFrames<1>(m, in, out, frames); return;
        case 2: rotateFrames<2>(m, in, out, frames); return;
        case 3: rotateFrames<3>(m, in, out, frames); return;
        case 4: rotateFrames<4>(m, in, out, frames); return;
        case 5: rotateFrames<5>(m, in, out, frames); return;
        case 6: rotateFrames<6>(m, in, out, frames); return;
        case 7: rotateFrames<7>(m, in, out, frames); return;
        default: break;
    }
    const int channels = numChannels(order);
    for (int f = 0; f < frames; ++f)
        for (int n = 0; n <= order; ++n)
            applyBlockAnyWidth(2 * n + 1, m + rotationOffset(n), in + f * channels + n * n, out + f * channels + n * n);
}

namespace {

inline void axialBlock(int w, const float* m, const float* in, float* out) {
    switch (w) {
        case 1: applyBlock<1>(m, in, out); return;
        case 2: applyBlock<2>(m, in, out); return;
        case 3: applyBlock<3>(m, in, out); return;
        case 4: applyBlock<4>(m, in, out); return;
        case 5: applyBlock<5>(m, in, out); return;
        case 6: applyBlock<6>(m, in, out); return;
        case 7: applyBlock<7>(m, in, out); return;
        case 8: applyBlock<8>(m, in, out); return;
        default: applyBlockAnyWidth(w, m, in, out); return;
    }
}

}  // namespace

void applyAxial(std::span<const float> blocks, std::span<const float> cosM, std::span<const float> sinM, int order,
                const float* in, float* out, int frames) noexcept {
    const int N = order, channels = numChannels(N);
    const bool turn = !cosM.empty();
    float gp[36], gn[36], yp[36], yn[36];  // a block is at most order + 1 wide; core's harmonics stop at 35
    for (int f = 0; f < frames; ++f) {
        const float* x = in + f * channels;
        float* y = out + f * channels;
        for (int m = 0; m <= N; ++m) {
            //  The channels of one m are not neighbours in ACN, so they are gathered, the block applied
            //  to the +m set and to the -m set, and the results put back -- turned, if there is a turn.
            const int w = N - m + 1;
            const float* B = blocks.data() + axialOffset(N, m);
            for (int i = 0; i < w; ++i) {
                const int n = m + i;
                gp[i] = x[n * n + n + m];
            }
            axialBlock(w, B, gp, yp);
            if (m == 0) {
                for (int i = 0; i < w; ++i) y[i * i + i] = yp[i];
                continue;
            }
            for (int i = 0; i < w; ++i) {
                const int n = m + i;
                gn[i] = x[n * n + n - m];
            }
            axialBlock(w, B, gn, yn);
            const float c = turn ? cosM[static_cast<std::size_t>(m)] : 1.0f,
                        s = turn ? sinM[static_cast<std::size_t>(m)] : 0.0f;
            for (int i = 0; i < w; ++i) {
                const int n = m + i;
                y[n * n + n + m] = yp[i] * c - yn[i] * s;
                y[n * n + n - m] = yn[i] * c + yp[i] * s;
            }
        }
    }
}

void axialBlocks(std::span<const double> rowMajor, int order, std::span<float> columnMajor) noexcept {
    for (int m = 0; m <= order; ++m) {
        const int w = order - m + 1, at = axialOffset(order, m);
        for (int i = 0; i < w; ++i)
            for (int j = 0; j < w; ++j)
                columnMajor[static_cast<std::size_t>(at + w * j + i)] =
                    static_cast<float>(rowMajor[static_cast<std::size_t>(at + w * i + j)]);
    }
}

void spinField(std::span<const float> cosM, std::span<const float> sinM, int order, float* data, int frames) noexcept {
    const int channels = numChannels(order);
    for (int f = 0; f < frames; ++f) {
        float* frame = data + f * channels;
        for (int n = 1; n <= order; ++n) {
            const int centre = n * n + n;
            for (int m = 1; m <= n; ++m) {
                //  The +m channel goes as cos(m az) and the -m channel as sin(m az); turning the field by
                //  the angle is adding it to every source's azimuth.
                const float c = cosM[static_cast<std::size_t>(m)], s = sinM[static_cast<std::size_t>(m)];
                const float plus = frame[centre + m], minus = frame[centre - m];
                frame[centre + m] = plus * c - minus * s;
                frame[centre - m] = minus * c + plus * s;
            }
        }
    }
}

void orderGains(std::span<const float> gains, int order, float* data, int frames) noexcept {
    const int channels = numChannels(order);
    for (int f = 0; f < frames; ++f) {
        float* frame = data + f * channels;
        for (int n = 0; n <= order; ++n) {
            const float g = gains[static_cast<std::size_t>(n)];
            for (int c = n * n; c < (n + 1) * (n + 1); ++c) frame[c] *= g;
        }
    }
}

}  // namespace bambi
