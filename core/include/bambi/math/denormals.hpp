// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define BAMBI_DENORMALS_SSE 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define BAMBI_DENORMALS_ARM64 1
#endif

/*  Denormals flushed to zero for as long as this lives, and the previous state put back after.
 *
 *  The plugins' processing and the render tools both open one of these, so a feedback path
 *  decays to the same zeros in a golden as in a bounce. x86: FTZ and DAZ (MXCSR 0x8040).
 *  arm64: FZ (FPCR bit 24). The same bits JUCE's ScopedNoDenormals sets. Elsewhere, nothing.
 */
namespace bambi {

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(BAMBI_DENORMALS_SSE)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);
#elif defined(BAMBI_DENORMALS_ARM64)
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        const std::uint64_t flushed = saved_ | (std::uint64_t{1} << 24);
        asm volatile("msr fpcr, %0" : : "r"(flushed));
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(BAMBI_DENORMALS_SSE)
        _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(BAMBI_DENORMALS_ARM64)
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t saved_{0};
};

}  // namespace bambi
