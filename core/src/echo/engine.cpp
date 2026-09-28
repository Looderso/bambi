// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/engine.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/echo/timing.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {
namespace {

constexpr float kRingLimit = 1.0e4f;   // +80 dBFS: a loop that grows is held here, and counted
constexpr float kRingTiny = 1.0e-15f;  // below this the ring holds exact silence
constexpr float kInputLimit = 1.0e6f;

//  What may go round a loop: finite, bounded, and not denormal. Written as selects because
//  std::max and std::clamp pass a NaN through.
inline float guarded(float v, int& held) {
    v = v == v ? v : 0.0f;
    const bool over = v > kRingLimit, under = v < -kRingLimit;
    held += static_cast<int>(over) + static_cast<int>(under);
    v = over ? kRingLimit : v;
    v = under ? -kRingLimit : v;
    return std::abs(v) < kRingTiny ? 0.0f : v;
}

}  // namespace

void EchoEngine::prepare(int order, double sampleRate) {
    order_ = order;
    channels_ = numChannels(order);
    sampleRate_ = sampleRate;
    capacity_ = static_cast<int>(std::ceil(maxTapSeconds(order) * sampleRate)) + kEchoChunk;
    line_.prepare(channels_, capacity_);
    work_.prepare(order, kEchoChunk);
    send_.prepare(order, kEchoChunk);
    sendSet_ = false;
    sendAmount_.jumpTo(1.0);
    dry_.jumpTo(0.0);
    wet_.jumpTo(1.0);
    const auto chunk = static_cast<std::size_t>(channels_ * kEchoChunk);
    x_.assign(chunk, 0.0f);
    older_.assign(chunk, 0.0f);
    fed_.assign(chunk, 0.0f);
    clean_.assign(chunk, 0.0f);
    sent_.assign(chunk, 0.0f);
    for (Tap& t : taps_) {
        t.ring.prepare(channels_, capacity_);
        prepareTap(t.transform, order);
        t.band.prepare(order);
        t.awake = t.on = false;
        t.level.jumpTo(0.0);
        t.feedback.jumpTo(0.0);
    }
    clamped_ = 0;
    reset();
}

void EchoEngine::reset() noexcept {
    line_.restart();
    for (Tap& t : taps_) {
        t.ring.restart();
        t.band.clear();
    }
    snap_ = true;
    sinceFrame_ = kEchoHop;
}

void EchoEngine::setFrame(const EchoFrame& frame) noexcept {
    if (!sendSet_ || !(frame.send == sendNow_)) {
        send_.set(frame.send);
        sendNow_ = frame.send;
        sendSet_ = true;
    }
    //  Where the hop that is ending has got to: all the way, unless the frame came early.
    const double w = rampWeight(sinceFrame_, kEchoHop);
    const auto retarget = [&](Ramp<double>& r, double target) {
        if (snap_)
            r.jumpTo(target);
        else
            r.glideTo(target, w);
    };
    retarget(sendAmount_, std::max(0.0, frame.sendAmount));
    retarget(dry_, std::max(0.0, frame.dryGain));
    retarget(wet_, std::max(0.0, frame.wetGain));

    for (int k = 0; k < kEchoTaps; ++k) {
        Tap& t = taps_[static_cast<std::size_t>(k)];
        const EchoTapFrame& f = frame.taps[static_cast<std::size_t>(k)];

        double level = t.level.at(w);
        const double feedback = t.feedback.at(w);
        t.previousPeriod = t.period;

        //  Off and faded out: sleep, and forget, so it wakes clean.
        if (t.awake && !t.on && level == 0.0) {
            t.awake = false;
            t.ring.restart();
            t.band.clear();
        }

        t.on = f.on;
        if (f.on && !t.awake) {
            t.awake = true;
            level = 0.0;  // it rises from nothing, over the hop
        }
        const int most = capacity_ - kEchoChunk;
        const int period = std::clamp(f.periodSamples, 1, most);
        t.offset = std::clamp(f.offsetSamples, 0, most);
        if (!t.awake || snap_) t.previousPeriod = period;
        t.period = period;

        const double levelTo = f.on && std::isfinite(f.level) ? std::max(0.0, f.level) : 0.0;
        const double feedbackTo =
            f.feedback == f.feedback ? std::clamp(f.feedback, 0.0, 1.0) : 0.0;  // clamp passes a NaN through
        if (snap_) {
            t.level.jumpTo(levelTo);
            t.feedback.jumpTo(feedbackTo);
        } else {
            t.level.glide(level, levelTo);
            t.feedback.glide(feedback, feedbackTo);
        }
        if (t.awake) buildTap(f.pass, sampleRate_, work_, t.transform);
    }
    snap_ = false;
    sinceFrame_ = 0;
}

void EchoEngine::process(const float* in, float* out, int frames) noexcept {
    const int C = channels_;
    int done = 0;
    while (done < frames) {
        //  A chunk is no longer than the shortest period in play, so it never reads what it writes.
        int n = std::min(frames - done, kEchoChunk);
        //  Nor does it straddle the end of a hop, so per-chunk decisions are independent of block size.
        if (sinceFrame_ < kEchoHop) n = std::min(n, kEchoHop - sinceFrame_);
        for (const Tap& t : taps_)
            if (t.awake) n = std::min({n, t.period, t.previousPeriod});

        const float* src = in + static_cast<std::size_t>(done) * static_cast<std::size_t>(C);
        for (int i = 0; i < n * C; ++i) {
            const float v = src[i];
            clean_[static_cast<std::size_t>(i)] = (v == v && std::abs(v) < kInputLimit) ? v : 0.0f;
        }
        send_.apply(clean_.data(), sent_.data(), n);
        if (!sendAmount_.holds(1.0))
            for (int f = 0; f < n; ++f) {
                const auto amount = static_cast<float>(sendAmount_.at(rampWeight(sinceFrame_ + f + 1, kEchoHop)));
                for (int c = 0; c < C; ++c) sent_[static_cast<std::size_t>(f * C + c)] *= amount;
            }
        line_.write(sent_.data(), n);

        float* dst = out + static_cast<std::size_t>(done) * static_cast<std::size_t>(C);
        std::fill(dst, dst + n * C, 0.0f);

        for (Tap& t : taps_) {
            if (!t.awake) continue;
            //  While the period changes, crossfade from the old read position over the hop.
            t.ring.fetch(t.period, n, x_.data());
            const bool moving = t.period != t.previousPeriod && sinceFrame_ < kEchoHop;
            if (moving) {
                t.ring.fetch(t.previousPeriod, n, older_.data());
                for (int f = 0; f < n; ++f) {
                    const auto w = static_cast<float>(rampWeight(sinceFrame_ + f + 1, kEchoHop));
                    for (int c = 0; c < C; ++c) {
                        const auto i = static_cast<std::size_t>(f * C + c);
                        x_[i] = older_[i] + (x_[i] - older_[i]) * w;
                    }
                }
            }
            onePass(t.transform, t.band, work_, x_.data(), n);

            line_.fetch(n + t.offset, n, fed_.data());
            int held = 0;
            for (int f = 0; f < n; ++f) {
                const double w = rampWeight(sinceFrame_ + f + 1, kEchoHop);
                const auto level = static_cast<float>(t.level.at(w));
                const auto feedback = static_cast<float>(t.feedback.at(w));
                for (int c = 0; c < C; ++c) {
                    const auto i = static_cast<std::size_t>(f * C + c);
                    const float y = x_[i];
                    dst[i] += level * y;
                    fed_[i] = guarded(fed_[i] + feedback * y, held);
                }
            }
            clamped_ += held;
            t.ring.write(fed_.data(), n);
        }

        //  The output stage. The dry path reads `clean_`, so no NaN reaches the output through it.
        if (!wet_.holds(1.0))
            for (int f = 0; f < n; ++f) {
                const auto wg = static_cast<float>(wet_.at(rampWeight(sinceFrame_ + f + 1, kEchoHop)));
                for (int c = 0; c < C; ++c) dst[static_cast<std::size_t>(f * C + c)] *= wg;
            }
        if (!dry_.holds(0.0))
            for (int f = 0; f < n; ++f) {
                const auto dg = static_cast<float>(dry_.at(rampWeight(sinceFrame_ + f + 1, kEchoHop)));
                for (int c = 0; c < C; ++c) {
                    const auto i = static_cast<std::size_t>(f * C + c);
                    dst[i] += dg * clean_[i];
                }
            }

        sinceFrame_ = std::min(kEchoHop, sinceFrame_ + n);
        done += n;
    }
}

}  // namespace bambi
