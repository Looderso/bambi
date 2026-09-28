// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>

struct PFFFT_Setup;

namespace bambi {

/*  Real FFT — a thin RAII wrapper around pffft.
 *
 *  A hand-rolled FFT is not worth writing here: pffft is BSD-style FFTPACK licensed, one C
 *  file, SIMD (SSE/NEON), single precision -- more than enough for control-rate feature
 *  analysis and what the SIMD path wants.
 */
class RealFft {
public:
    RealFft() = default;
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;
    RealFft(RealFft&&) noexcept;
    RealFft& operator=(RealFft&&) noexcept;

    /// `size` must be a multiple of 32 and factor into 2s, 3s and 5s. Allocates.
    void prepare(std::size_t size);

    /// Magnitude spectrum of `in` (size samples) into `mag` (size/2 + 1 bins).
    /// No allocation, no locks.
    void magnitude(std::span<const double> in, std::span<double> mag);

    /*  Power spectrum |X|^2 into `pw` (size/2 + 1 bins). Prefer this over magnitude()
     *  wherever the caller squares the result again or only needs ratios: a sqrt per bin
     *  followed by a multiply per bin is 2048 wasted operations per frame at size 2048.
     *  Callers that genuinely need magnitude in a sub-range can sqrt just that range. */
    void power(std::span<const double> in, std::span<double> pw);

    std::size_t size() const { return n_; }
    std::size_t bins() const { return n_ / 2 + 1; }

private:
    void release();

    PFFFT_Setup* setup_{nullptr};
    std::size_t n_{0};
    float* in_{nullptr};
    float* out_{nullptr};
    float* work_{nullptr};
};

/// Periodic Hann window, matching scipy's. The window shape moves band edges, so the
/// feature calibration travels with it.
void hannWindow(std::span<double> w);

}  // namespace bambi
