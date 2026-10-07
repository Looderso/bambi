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

bool RealFft::prepare(std::size_t size) {
    release();
    n_ = size;
    setup_ = pffft_new_setup(static_cast<int>(n_), PFFFT_REAL);
    if (!setup_) {
        n_ = 0;
        return false;
    }
    in_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    out_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    work_ = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    return true;
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

void RealFft::forward(std::span<const double> in, std::span<double> re, std::span<double> im) {
    if (!setup_ || in.size() < n_ || re.size() < bins() || im.size() < bins()) return;

    for (std::size_t i = 0; i < n_; ++i) in_[i] = static_cast<float>(in[i]);
    pffft_transform_ordered(setup_, in_, out_, work_, PFFFT_FORWARD);

    const std::size_t half = n_ / 2;
    re[0] = out_[0];  // pffft packs the two real bins into the first complex slot
    im[0] = 0.0;
    re[half] = out_[1];
    im[half] = 0.0;
    for (std::size_t k = 1; k < half; ++k) {
        re[k] = out_[2 * k];
        im[k] = out_[2 * k + 1];
    }
}

void RealFft::inverse(std::span<const double> re, std::span<const double> im, std::span<double> out) {
    if (!setup_ || re.size() < bins() || im.size() < bins() || out.size() < n_) return;

    const std::size_t half = n_ / 2;
    in_[0] = static_cast<float>(re[0]);
    in_[1] = static_cast<float>(re[half]);
    for (std::size_t k = 1; k < half; ++k) {
        in_[2 * k] = static_cast<float>(re[k]);
        in_[2 * k + 1] = static_cast<float>(im[k]);
    }
    pffft_transform_ordered(setup_, in_, out_, work_, PFFFT_BACKWARD);

    const double scale = 1.0 / static_cast<double>(n_);  // pffft's backward transform is unscaled
    for (std::size_t i = 0; i < n_; ++i) out[i] = out_[i] * scale;
}

void RealFft::spectrum(const float* in, float* spec) noexcept {
    if (setup_) pffft_transform(setup_, in, spec, work_, PFFFT_FORWARD);
}

void RealFft::signal(const float* spec, float* out) noexcept {
    if (setup_) pffft_transform(setup_, spec, out, work_, PFFFT_BACKWARD);
}

void RealFft::convolveAccumulate(const float* a, const float* b, float* ab, float scale) const noexcept {
    if (setup_) pffft_zconvolve_accumulate(setup_, a, b, ab, scale);
}

AlignedFloats::~AlignedFloats() {
    if (p_) pffft_aligned_free(p_);
}

AlignedFloats& AlignedFloats::operator=(AlignedFloats&& o) noexcept {
    if (this != &o) {
        if (p_) pffft_aligned_free(p_);
        p_ = std::exchange(o.p_, nullptr);
        n_ = std::exchange(o.n_, 0);
    }
    return *this;
}

void AlignedFloats::prepare(std::size_t n) {
    if (p_) pffft_aligned_free(p_);
    p_ = n ? static_cast<float*>(pffft_aligned_malloc(n * sizeof(float))) : nullptr;
    n_ = p_ ? n : 0;
    clear();
}

void AlignedFloats::clear() noexcept {
    for (std::size_t i = 0; i < n_; ++i) p_[i] = 0.0f;
}

void hannWindow(std::span<double> w) {
    const std::size_t n = w.size();
    for (std::size_t i = 0; i < n; ++i)
        w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
}

}  // namespace bambi
