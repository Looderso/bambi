// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/decode/binaural.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/link/latest.hpp"

/*  The decoder's convolution: (order + 1)^2 channels in, two ears out, one FIR per channel and ear,
 *  run as a uniformly partitioned convolution in the frequency domain. Input is gathered in blocks
 *  of one partition, so the output is one partition late -- latency(), for the host to take back.
 *  The filters' own bulk delay stays inside the taps: it is the head's, not the plugin's.
 *
 *  A finished set of filters arrives whole from a worker: cut into partitions and transformed there,
 *  then handed over through a triple buffer with one atomic exchange, so the audio thread never reads
 *  a slot being written and copies nothing. On the block where a new set arrives, that block is
 *  convolved with both sets and the two results crossfaded over the block, so nothing steps.
 */
namespace bambi {

class BinauralConvolver {
public:
    /// Allocates: three sets of (order + 1)^2 x 2 filters of `taps`, cut into pieces of `partition` samples, and
    /// the histories. `partition` is a power of two of at least 32 (the FFT is twice it); taps round up to whole
    /// partitions. False when the sizes do not hold; the convolver is then inert. Never while publish() or process()
    /// may run: it reallocates under both.
    bool prepare(int order, int taps, int partition = 64);

    int latency() const { return partition_; }  ///< samples
    int channels() const { return channels_; }
    int partitions() const { return parts_; }

    /// Worker thread, one at a time: transforms a builder's filters (channels() equal to this one's, at most taps)
    /// into the staging set and publishes it. Allocates nothing. False, and nothing published, when they do not fit.
    bool publish(const BinauralFilterBuilder& filters) noexcept;

    /// Audio thread. `in`: `frames` interleaved frames of channels() values; `out`: `frames` interleaved stereo
    /// frames, the left ear first; they may not alias. Silence until a set was published.
    void process(const float* in, float* out, int frames) noexcept;
    /// Forgets the input and output histories, keeps the filters.
    void reset() noexcept;

private:
    /// Every filter's partitions as spectra: channels x 2 x partitions x fft, in the FFT's own layout.
    struct Set {
        AlignedFloats spectra;
    };
    const float* spectrumOf(const Set& set, int channel, int ear, int part) const noexcept;
    float* historySlot(int channel, int part) noexcept;
    /// The current block through `set`: `out` gets 2 x partition samples, the left ear first.
    void convolveBlock(const Set& set, float* out) noexcept;
    void runBlock() noexcept;

    int channels_{0}, taps_{0}, partition_{0}, fft_{0}, parts_{0};
    RealFft audioFft_, workerFft_;
    LatestValue<Set> sets_;
    const Set* live_{nullptr};
    AlignedFloats input_;      // channels x fft: the previous block, then the current one
    AlignedFloats history_;    // channels x partitions x fft: the spectra of the last blocks, a ring
    int newest_{0};            // the ring slot the newest block went to
    AlignedFloats acc_, ear_;  // fft each: one ear's accumulated spectrum and its signal
    AlignedFloats piece_;      // fft: worker scratch, one partition's taps padded
    std::vector<float> playing_;  // 2 x partition: what is being played during the current block
    std::vector<float> leaving_;  // 2 x partition: the old set's block, while a new set fades in
    int pos_{0};                  // frames of the current block gathered so far
};

}  // namespace bambi
