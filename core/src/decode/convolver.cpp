// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/convolver.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }
bool powerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }
}  // namespace

bool BinauralConvolver::prepare(int order, int taps, int partition) {
    live_ = nullptr;
    piece_.prepare(0);  // inert until everything below held: publish() and process() look at this buffer's size
    if (order < 0 || order > kMaxOrder || taps <= 0 || !powerOfTwo(partition) || partition < 32) return false;
    channels_ = numChannels(order);
    partition_ = partition;
    fft_ = 2 * partition;
    parts_ = (taps + partition - 1) / partition;
    taps_ = parts_ * partition;
    if (!audioFft_.prepare(sz(fft_)) || !workerFft_.prepare(sz(fft_))) return false;

    const std::size_t perSet = sz(channels_) * 2 * sz(parts_) * sz(fft_);
    for (int i = 0; i < 3; ++i) {  // every slot of the triple buffer passes through the writer's hands once
        sets_.staging().spectra.prepare(perSet);
        sets_.publish();
        (void)sets_.latest();  // and the reader takes it, so the next publish reaches another slot
    }
    input_.prepare(sz(channels_) * sz(fft_));
    history_.prepare(sz(channels_) * sz(parts_) * sz(fft_));
    acc_.prepare(sz(fft_));
    ear_.prepare(sz(fft_));
    piece_.prepare(sz(fft_));
    playing_.assign(2 * sz(partition_), 0.0f);
    leaving_.assign(2 * sz(partition_), 0.0f);
    pos_ = 0;
    newest_ = 0;
    return true;
}

const float* BinauralConvolver::spectrumOf(const Set& set, int channel, int ear, int part) const noexcept {
    return set.spectra.data() + ((sz(channel) * 2 + sz(ear)) * sz(parts_) + sz(part)) * sz(fft_);
}

float* BinauralConvolver::historySlot(int channel, int part) noexcept {
    return history_.data() + (sz(channel) * sz(parts_) + sz(part)) * sz(fft_);
}

bool BinauralConvolver::publish(const BinauralFilterBuilder& filters) noexcept {
    if (filters.channels() != channels_ || filters.taps() > taps_ || piece_.size() == 0) return false;
    Set& set = sets_.staging();
    for (int c = 0; c < channels_; ++c)
        for (int e = 0; e < 2; ++e) {
            const std::span<const float> taps = filters.filter(c, e);
            for (int j = 0; j < parts_; ++j) {
                // A partition's taps in the first half, zeros in the second: the shape overlap-save needs.
                float* piece = piece_.data();
                for (int i = 0; i < partition_; ++i) {
                    const std::size_t t = sz(j * partition_ + i);
                    piece[i] = t < taps.size() ? taps[t] : 0.0f;
                }
                std::fill(piece + partition_, piece + fft_, 0.0f);
                workerFft_.spectrum(piece, set.spectra.data() + ((sz(c) * 2 + sz(e)) * sz(parts_) + sz(j)) * sz(fft_));
            }
        }
    sets_.publish();
    return true;
}

void BinauralConvolver::convolveBlock(const Set& set, float* out) noexcept {
    const float scale = 1.0f / static_cast<float>(fft_);
    for (int e = 0; e < 2; ++e) {
        acc_.clear();
        for (int c = 0; c < channels_; ++c)
            for (int j = 0; j < parts_; ++j) {
                const int slot = (newest_ - j + parts_) % parts_;  // the block j partitions ago
                audioFft_.convolveAccumulate(historySlot(c, slot), spectrumOf(set, c, e, j), acc_.data(), scale);
            }
        audioFft_.signal(acc_.data(), ear_.data());
        // Overlap-save: the second half of the circular result is the linear one.
        std::copy(ear_.data() + partition_, ear_.data() + fft_, out + sz(e) * sz(partition_));
    }
}

void BinauralConvolver::runBlock() noexcept {
    newest_ = (newest_ + 1) % parts_;
    for (int c = 0; c < channels_; ++c) audioFft_.spectrum(input_.data() + sz(c) * sz(fft_), historySlot(c, newest_));

    bool fade = false;
    if (live_ == nullptr) {
        live_ = sets_.latest();
    } else if (sets_.hasFresh()) {
        convolveBlock(*live_, leaving_.data());  // the old set's last word, before its slot goes back
        live_ = sets_.latest();
        fade = true;
    }
    if (live_ != nullptr) convolveBlock(*live_, playing_.data());
    else std::fill(playing_.begin(), playing_.end(), 0.0f);
    if (fade)
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < partition_; ++i) {
                const float t = static_cast<float>(i + 1) / static_cast<float>(partition_);
                float& v = playing_[sz(e) * sz(partition_) + sz(i)];
                v = leaving_[sz(e) * sz(partition_) + sz(i)] * (1.0f - t) + v * t;
            }

    // This block becomes the previous one.
    for (int c = 0; c < channels_; ++c) {
        float* x = input_.data() + sz(c) * sz(fft_);
        std::copy(x + partition_, x + fft_, x);
    }
}

void BinauralConvolver::process(const float* in, float* out, int frames) noexcept {
    if (piece_.size() == 0) return;
    for (int f = 0; f < frames; ++f) {
        const float* x = in + sz(f) * sz(channels_);
        for (int c = 0; c < channels_; ++c) {
            const float v = x[c];
            input_.data()[sz(c) * sz(fft_) + sz(partition_ + pos_)] = (v == v && std::abs(v) < 1.0e6f) ? v : 0.0f;
        }
        out[sz(f) * 2] = playing_[sz(pos_)];
        out[sz(f) * 2 + 1] = playing_[sz(partition_ + pos_)];
        if (++pos_ == partition_) {
            runBlock();
            pos_ = 0;
        }
    }
}

void BinauralConvolver::reset() noexcept {
    input_.clear();
    history_.clear();
    std::fill(playing_.begin(), playing_.end(), 0.0f);
    pos_ = 0;
}

}  // namespace bambi
