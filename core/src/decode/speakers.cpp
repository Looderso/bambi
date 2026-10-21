// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/speakers.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/dsp/guard.hpp"
#include "bambi/dsp/ramp.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }
}  // namespace

bool SpeakerRenderer::prepare(int maxOrder, int maxOutputs, double sampleRate, double maxDelaySeconds,
                              int latencyFrames) {
    live_ = nullptr;
    fadePos_ = -1;
    fading_ = false;
    settle_ = 0;
    ringLive_.clear();
    if (maxOrder < 0 || maxOrder > kMaxOrder || maxOutputs < 1 || !(sampleRate > 0) || !(maxDelaySeconds >= 0))
        return false;
    maxOutputs_ = maxOutputs;
    maxChannels_ = numChannels(maxOrder);
    sampleRate_ = sampleRate;
    maxDelay_ = static_cast<int>(std::lround(maxDelaySeconds * sampleRate));
    latency_ = std::max(0, latencyFrames);
    ringLength_ = 1;
    while (ringLength_ <= maxDelay_ + latency_) ringLength_ *= 2;
    fade_ = std::max(1, static_cast<int>(std::lround(kSpeakerFadeSeconds * sampleRate)));

    auto room = [&](Set& s) {
        s.outputs = s.channels = 0;
        s.matrix.assign(sz(maxOutputs_) * sz(maxChannels_), 0.0f);
        s.delay.assign(sz(maxOutputs_), 0);
    };
    for (int i = 0; i < 3; ++i) {  // every slot of the triple buffer passes through the writer's hands once
        room(sets_.staging());
        sets_.publish();
        (void)sets_.latest();
    }
    room(leaving_);
    ringLive_.assign(sz(maxOutputs_) * sz(ringLength_), 0.0f);
    ringLeave_.assign(sz(maxOutputs_) * sz(ringLength_), 0.0f);
    yLive_.assign(sz(maxOutputs_), 0.0f);
    yLeave_.assign(sz(maxOutputs_), 0.0f);
    write_ = 0;
    return true;
}

bool SpeakerRenderer::publish(const SpeakerDesign& design) noexcept {
    if (ringLive_.empty() || design.outputs > maxOutputs_ || design.outputs < 1 ||
        numChannels(design.order) > maxChannels_)
        return false;
    const int C = numChannels(design.order);
    for (double d : design.delaySeconds)
        if (!(d >= 0) || std::lround(d * sampleRate_) > maxDelay_) return false;
    if (design.matrix.size() != sz(design.outputs) * sz(C)) return false;

    Set& set = sets_.staging();
    set.outputs = design.outputs;
    set.channels = C;
    std::fill(set.matrix.begin(), set.matrix.end(), 0.0f);
    std::fill(set.delay.begin(), set.delay.end(), 0);
    //  The rows as designed; then each loudspeaker's distance gain into its row, and its delay in whole samples.
    for (int o = 0; o < design.outputs; ++o)
        for (int c = 0; c < C; ++c)
            set.matrix[sz(c) * sz(maxOutputs_) + sz(o)] = static_cast<float>(design.matrix[sz(o) * sz(C) + sz(c)]);
    set.longestDelay = 0;
    for (std::size_t i = 0; i < design.channel.size(); ++i) {
        const int o = design.channel[i];
        if (o < 0 || o >= design.outputs) return false;
        for (int c = 0; c < C; ++c) set.matrix[sz(c) * sz(maxOutputs_) + sz(o)] *= static_cast<float>(design.gain[i]);
        set.delay[sz(o)] = static_cast<int>(std::lround(design.delaySeconds[i] * sampleRate_)) + latency_;
        set.longestDelay = std::max(set.longestDelay, set.delay[sz(o)]);
    }
    sets_.publish();
    return true;
}

void SpeakerRenderer::mix(const Set& set, const float* x, int inChannels, float* y) const noexcept {
    const int C = std::min(set.channels, inChannels), O = set.outputs;
    std::fill(y, y + maxOutputs_, 0.0f);
    for (int c = 0; c < C; ++c) {
        const float* row = set.matrix.data() + sz(c) * sz(maxOutputs_);
        const float xc = x[c];
        for (int o = 0; o < O; ++o) y[o] += row[o] * xc;
    }
}

void SpeakerRenderer::process(const float* in, int inChannels, float* out, int outChannels, int frames) noexcept {
    if (ringLive_.empty()) return;
    //  A new design is taken up between blocks, and only when no fade is running: the old one is copied first, since
    //  its slot goes back to the worker with latest().
    if (!fading_ && settle_ == 0 && sets_.hasFresh()) {
        const bool had = live_ != nullptr;
        if (had) {
            leaving_.outputs = live_->outputs;
            leaving_.channels = live_->channels;
            std::copy(live_->matrix.begin(), live_->matrix.end(), leaving_.matrix.begin());
            std::copy(live_->delay.begin(), live_->delay.end(), leaving_.delay.begin());
        }
        live_ = sets_.latest();
        if (had) {
            fading_ = true;
            //  The ramp starts once the new design's lines hold its own signal: what a line held from before -- a
            //  wider design that left it, say -- lies further back than any read of it that is heard.
            fadePos_ = -live_->longestDelay;
        }
    }
    const int mask = ringLength_ - 1;
    for (int f = 0; f < frames; ++f) {
        const float* x = in + sz(f) * sz(inChannels);
        float* o = out + sz(f) * sz(outChannels);
        if (live_ == nullptr) {
            std::fill(o, o + outChannels, 0.0f);
            continue;
        }
        mix(*live_, x, inChannels, yLive_.data());
        const bool fading = fading_;
        //  t is the live design's share of the output: 0 while its lines fill, then the ramp, then 1
        const float t = !fading ? 1.0f : fadePos_ < 0 ? 0.0f : static_cast<float>(rampWeight(fadePos_ + 1, fade_));
        if (fading) mix(leaving_, x, inChannels, yLeave_.data());
        //  Only the outputs a design feeds are written and read, so a pair costs less than a dome.
        const int active = std::max(live_->outputs, fading ? leaving_.outputs : 0);
        for (int k = 0; k < active; ++k) {
            const float v = yLive_[sz(k)], w = fading ? yLeave_[sz(k)] : v;
            float* live = ringLive_.data() + sz(k) * sz(ringLength_);
            float* leave = ringLeave_.data() + sz(k) * sz(ringLength_);
            live[write_] = cleanSample(v);
            leave[write_] = cleanSample(w);
            float heard = live[(write_ - live_->delay[sz(k)]) & mask];
            if (fading) heard = leave[(write_ - leaving_.delay[sz(k)]) & mask] * (1.0f - t) + heard * t;
            if (k < outChannels) o[k] = heard;
        }
        for (int k = active; k < outChannels; ++k) o[k] = 0.0f;
        write_ = (write_ + 1) & mask;
        if (fading && ++fadePos_ >= fade_) {
            fading_ = false;
            fadePos_ = -1;
            settle_ = live_->longestDelay;  // the leaving lines still hold the old design this far back
        } else if (!fading && settle_ > 0) {
            --settle_;
        }
    }
}

void SpeakerRenderer::reset() noexcept {
    std::fill(ringLive_.begin(), ringLive_.end(), 0.0f);
    std::fill(ringLeave_.begin(), ringLeave_.end(), 0.0f);
    write_ = 0;
}

bool SpeakerDecoder::prepare(int maxOrder, int maxOutputs, double sampleRate, double maxDelaySeconds,
                             int latencyFrames) {
    return renderer_.prepare(maxOrder, maxOutputs, sampleRate, maxDelaySeconds, latencyFrames);
}

void SpeakerDecoder::request(const Layout& layout, int order, Weighting weighting, const Microphones& microphones) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        layout_ = layout;
        order_ = order;
        weighting_ = weighting;
        microphones_ = microphones;
    }
    requested_.fetch_add(1, std::memory_order_release);
}

bool SpeakerDecoder::work() {
    //  The count is read before the request: one that lands in between leaves the count ahead of what was built, so
    //  the next work() designs again.
    const int seen = requested_.load(std::memory_order_acquire);
    if (seen == built_.load(std::memory_order_relaxed)) return false;
    Layout layout;
    int order;
    Weighting weighting;
    Microphones microphones;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        layout = layout_;
        order = order_;
        weighting = weighting_;
        microphones = microphones_;
    }
    SpeakerDesign design;
    std::string error;
    bool published = false;
    if (!designAllrad(layout, order, weighting, design, &error, microphones)) {
        //  `error` holds the reason; the previous design keeps playing
    } else if (!renderer_.publish(design)) {
        error = "the design does not fit what the renderer was prepared for";
    } else {
        design_ = std::move(design);
        published = true;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
    }
    built_.store(seen, std::memory_order_release);
    return published;
}

std::string SpeakerDecoder::lastError() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

}  // namespace bambi
