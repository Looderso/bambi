// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>
#include <span>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/math/sh.hpp"

/*  What a field effect listens to.
 *
 *  The encoder has a mono source and sums it. An effect has a field. Its loudness is the power of
 *  all its channels (`fieldPower`), which one source reads exactly as W alone would and which
 *  nothing can cancel; W alone reads a pair in antiphase as silence, and a ducker then lets the wet
 *  through under a loud field. Its spectrum is W's: tonal and the bands are ratios and need a wave,
 *  which a power is not. The sidechain is whatever a host sends, so it is summed from its first pair
 *  as the encoder's is.
 *
 *  It owns the two feature banks, because they are fed from exactly these two signals and from
 *  nothing else.
 */
namespace bambi::host {

class FieldInput {
public:
    /// Allocates: the three buffers and both banks. Nothing after this allocates.
    void prepare(double sampleRate, int maxBlock) {
        const auto n = static_cast<std::size_t>(std::max(1, maxBlock));
        mono_.assign(n, 0.0f);
        side_.assign(n, 0.0f);
        power_.assign(n, 0.0f);
        features_.prepare(sampleRate);
        sidechainFeatures_.prepare(sampleRate);
    }

    /// A start or a locate: the detectors hold no memory of a timeline that is no longer playing.
    void reset() {
        features_.reset();
        sidechainFeatures_.reset();
    }

    std::size_t capacity() const noexcept { return mono_.size(); }

    /*  Take this block's two signals. Returns false when the host exceeded the block size it
     *  announced, which is the caller's cue to output silence rather than resize on this thread.
     *  `order` is what the engine renders at: a host may hand over a bus wider than that, and
     *  whatever it left in the channels past (order + 1)^2 is not the field. */
    bool take(const juce::AudioBuffer<float>& field, const juce::AudioBuffer<float>* sidechain, int numSamples,
              int order) {
        const auto n = static_cast<std::size_t>(std::max(0, numSamples));
        if (n > mono_.size()) return false;

        peak_ = 0.0f;
        if (field.getNumChannels() > 0) {
            const float* w = field.getReadPointer(0);
            std::copy_n(w, n, mono_.begin());
            for (std::size_t i = 0; i < n; ++i) peak_ = std::max(peak_, std::abs(mono_[i]));

            std::array<const float*, bambi::kMaxChannels> channels{};
            //  whole orders only: the largest the bus can carry, and never more than the engine's
            int whole = std::clamp(order, 0, bambi::kMaxOrder);
            while (whole > 0 && bambi::numChannels(whole) > field.getNumChannels()) --whole;
            const int count = bambi::numChannels(whole);
            for (int c = 0; c < count; ++c) channels[static_cast<std::size_t>(c)] = field.getReadPointer(c);
            bambi::fieldPower(std::span<const float* const>(channels.data(), static_cast<std::size_t>(count)),
                              static_cast<int>(n), power_.data());
        } else {
            std::fill_n(mono_.begin(), n, 0.0f);
            std::fill_n(power_.begin(), n, 0.0f);
        }

        sidechainActive_ = sidechain != nullptr && sidechain->getNumChannels() > 0;
        sidePeak_ = 0.0f;
        std::fill_n(side_.begin(), n, 0.0f);
        if (sidechainActive_) {
            const int used = std::min(2, sidechain->getNumChannels());
            const float k = 1.0f / static_cast<float>(used);
            for (int c = 0; c < used; ++c) {
                const float* x = sidechain->getReadPointer(c);
                for (std::size_t i = 0; i < n; ++i) side_[i] += x[i] * k;
            }
            for (std::size_t i = 0; i < n; ++i) sidePeak_ = std::max(sidePeak_, std::abs(side_[i]));
        }
        return true;
    }

    /// Feed `frames` from `at` to the detectors. Real-time safe.
    void analyse(std::size_t at, std::size_t frames) {
        features_.process(mono_.data() + at, power_.data() + at, static_cast<int>(frames));
        if (sidechainActive_) sidechainFeatures_.process(side_.data() + at, static_cast<int>(frames));
    }

    /// How long each Level takes to fall, in milliseconds as the parameters have it.
    void setLevelRelease(float selfMs, float sidechainMs) noexcept {
        features_.setLevelRelease(static_cast<double>(selfMs) * 0.001);
        sidechainFeatures_.setLevelRelease(static_cast<double>(sidechainMs) * 0.001);
    }

    /// The six of each, in the Feature enum's order, as a control step wants them.
    void readFeatures(std::array<double, bambi::kNumFeatures>& self,
                      std::array<double, bambi::kNumFeatures>& side) const {
        for (int i = 0; i < bambi::kNumFeatures; ++i) {
            self[static_cast<std::size_t>(i)] = features_.value(static_cast<bambi::Feature>(i));
            side[static_cast<std::size_t>(i)] = sidechainFeatures_.value(static_cast<bambi::Feature>(i));
        }
    }

    bool sidechainActive() const noexcept { return sidechainActive_; }
    float peak() const noexcept { return peak_; }
    float sidechainPeak() const noexcept { return sidePeak_; }

private:
    std::vector<float> mono_, side_, power_;  ///< W, the sidechain's sum, the field's power
    bambi::FeatureBank features_, sidechainFeatures_;
    bool sidechainActive_{false};
    float peak_{0.0f}, sidePeak_{0.0f};
};

}  // namespace bambi::host
