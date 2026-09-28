// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/fft.hpp"

#include <cmath>
#include <utility>

#include "bambi/math/vec3.hpp"
#include "pffft.h"

namespace bambi {

RealFft::~RealFft() { release(); }

RealFft::RealFft(RealFft&& o) noexcept : setup_(o.setup_), n_(o.n_), in_(o.in_), out_(o.out_), work_(o.work_) {
    o.setup_ = nullptr;
    o.n_ = 0;
    o.in_ = o.out_ = o.work_ = nullptr;
}

RealFft& RealFft::operator=(RealFft&& o) noexcept {
    if (this != &o) {
        release();
        setup_ = std::exchange(o.setup_, nullptr);
        n_ = std::exchange(o.n_, 0);
        in_ = std::exchange(o.in_, nullptr);
        out_ = std::exchange(o.out_, nullptr);
        work_ = std::exchange(o.work_, nullptr);
    }
    return *this;
}

void RealFft::release() {
    if (setup_) pffft_destroy_setup(setup_);
    if (in_) pffft_aligned_free(in_);
    if (out_) pffft_aligned_free(out_);
    if (work_) pffft_aligned_free(work_);
    setup_ = nullptr;
    in_ = out_ = work_ = nullptr;
    n_ = 0;
}

void RealFft::prepare(std::size_t size) {
    release();
    n_ = size;
    setup_ = pffft_new_setup(static_cast<int>(n_), PFFFT_REAL);
    if (!setup_) {
        n_ = 0;
        return;
    }
    in_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    out_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    work_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
}

void RealFft::magnitude(std::span<const double> in, std::span<double> mag) {
    if (!setup_ || in.size() < n_ || mag.size() < bins()) return;

    for (std::size_t i = 0; i < n_; ++i) in_[i] = static_cast<float>(in[i]);
    pffft_transform_ordered(setup_, in_, out_, work_, PFFFT_FORWARD);

    // Ordered real output packs DC and Nyquist into the first two slots, then interleaved
    // (re, im) pairs for bins 1..n/2-1.
    const std::size_t half = n_ / 2;
    mag[0] = std::abs(static_cast<double>(out_[0]));
    mag[half] = std::abs(static_cast<double>(out_[1]));
    for (std::size_t k = 1; k < half; ++k) {
        const double re = out_[2 * k], im = out_[2 * k + 1];
        mag[k] = std::sqrt(re * re + im * im);
    }
}

void RealFft::power(std::span<const double> in, std::span<double> pw) {
    if (!setup_ || in.size() < n_ || pw.size() < bins()) return;

    for (std::size_t i = 0; i < n_; ++i) in_[i] = static_cast<float>(in[i]);
    pffft_transform_ordered(setup_, in_, out_, work_, PFFFT_FORWARD);

    const std::size_t half = n_ / 2;
    const double dc = out_[0], nyq = out_[1];
    pw[0] = dc * dc;
    pw[half] = nyq * nyq;
    for (std::size_t k = 1; k < half; ++k) {
        const double re = out_[2 * k], im = out_[2 * k + 1];
        pw[k] = re * re + im * im;
    }
}

void hannWindow(std::span<double> w) {
    const std::size_t n = w.size();
    for (std::size_t i = 0; i < n; ++i)
        w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
}

}  // namespace bambi
