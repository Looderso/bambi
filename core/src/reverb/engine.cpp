// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/engine.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"

namespace bambi {
namespace {

/*  The balance tuned by ear used a tail-level formula that leaves out what its diffuser does: only
 *  (1-a)^2 + a^2 of what goes in goes round, so the tail as heard is quieter than the model says.
 *  This keeps that balance. False makes the tail 1.7 to 3 dB louder, matching the model's energies. */
constexpr bool kBalanceAsHeard = true;

bool sameRoom(const RoomSettings& a, const RoomSettings& b) {
    return a.size == b.size && a.shape == b.shape && a.decay == b.decay && a.tone == b.tone &&
           a.roughness == b.roughness;
}

}  // namespace

void ReverbEngine::prepare(int busOrder, double sampleRate) {
    busOrder_ = busOrder;
    channels_ = numChannels(busOrder);
    fs_ = sampleRate;
    early_.prepare(busOrder, sampleRate);
    for (FdnTail& t : tail_) t.prepare(sampleRate);
    live_ = 0;
    fading_ = -1;
    havePending_ = false;
    played_ = false;
    leavingRatio_ = 1.0;
    planScale_ = planMixing_ = -1.0;
    fadeFrames_ = std::max(1, static_cast<int>(kTailFadeSeconds * sampleRate));
    const auto chunk = static_cast<std::size_t>(kChunk);
    clean_.assign(chunk * static_cast<std::size_t>(channels_), 0.0f);
    field_.assign(clean_.size(), 0.0f);
    placed_.assign(clean_.size(), 0.0f);
    returnOp_.prepare(busOrder, kChunk);
    regionsSet_ = false;
    returnPlain_ = true;
    sendSourceRaw_.fill(1.0f);
    sendLineRaw_.fill(1.0f);
    returnLineRaw_.fill(1.0f);
    for (auto& r : sendLine_) r.jumpTo(1.0f);
    for (auto& r : returnLine_) r.jumpTo(1.0f);
    scattered_.assign(chunk * kVirtualSources, 0.0f);
    lineIn_.assign(chunk * kMaxTailLines, 0.0f);
    lineScattered_.assign(lineIn_.size(), 0.0f);
    lineOut_.assign(lineIn_.size(), 0.0f);
    lineOutOld_.assign(lineIn_.size(), 0.0f);
    configured_ = false;
    snap_ = true;
    set(ReverbFrame{});
}

void ReverbEngine::reset() noexcept {
    early_.reset();
    for (FdnTail& t : tail_) t.reset();
    //  live_ is not reset: it names the network holding the current room, and moving it would play the
    //  room as it was against the room as it is set now. A fade in flight is dropped.
    fading_ = -1;
    havePending_ = false;
    played_ = false;
    leavingRatio_ = 1.0;
    snap_ = true;
    sinceSet_ = kReverbHop;
}

void ReverbEngine::layOutLines(int lines) noexcept {
    lines_ = lines;
    regionsSet_ = false;  ///< the lines' directions are where the regions are read
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int k = 0; k < lines; ++k) {
        const double z = 1.0 - 2.0 * (k + 0.5) / lines, r = std::sqrt(1.0 - z * z), a = k * golden;
        lineDirection_[static_cast<std::size_t>(k)] = {r * std::cos(a), r * std::sin(a), z};
    }
    //  Each line reads the bus at its own direction with an order-1 beam. Nothing on a bus knows where a
    //  source is, so the beams are scaled once, by what keeping a source's energy asks on average.
    const int order = std::min(busOrder_, 1);
    const double w1 = std::cos(137.9 * kDeg2Rad / (order + 1.51)), norm = order == 0 ? 1.0 : 1.0 + 3.0 * w1;
    double meanSquare = 0.0;
    const auto virtuals = virtualSourceDirections();
    for (const Vec3& u : virtuals)
        for (int k = 0; k < lines; ++k) {
            const double b = maxReBeam(order, dot(lineDirection_[static_cast<std::size_t>(k)], u));
            meanSquare += b * b / kVirtualSources;
        }
    const double scale = 1.0 / std::sqrt(std::max(meanSquare, 1e-12));
    std::array<double, kEarlyChannels> y{};
    for (int k = 0; k < lines; ++k) {
        const auto ki = static_cast<std::size_t>(k);
        shSN3D(lineDirection_[ki], std::max(order, 0), y);
        for (int c = 0; c < 4; ++c) {
            const int n = c == 0 ? 0 : 1;
            read_[ki][static_cast<std::size_t>(c)] =
                c < numChannels(order) ? static_cast<float>((2 * n + 1) * (n == 0 ? 1.0 : w1) *
                                                            y[static_cast<std::size_t>(c)] / norm * scale)
                                       : 0.0f;
        }
        //  what a virtual source scattered goes to the lines near it, its energy kept
        double sum = 0.0;
        for (int v = 0; v < kVirtualSources; ++v) {
            const double b =
                std::max(0.0, maxReBeam(1, dot(lineDirection_[ki], virtuals[static_cast<std::size_t>(v)])));
            fromScattered_[ki][static_cast<std::size_t>(v)] = static_cast<float>(b);
            sum += b * b;
        }
        (void)sum;
    }
    for (int v = 0; v < kVirtualSources; ++v) {
        double sum = 0.0;
        for (int k = 0; k < lines; ++k)
            sum += static_cast<double>(fromScattered_[static_cast<std::size_t>(k)][static_cast<std::size_t>(v)]) *
                   fromScattered_[static_cast<std::size_t>(k)][static_cast<std::size_t>(v)];
        const auto inv = static_cast<float>(1.0 / std::sqrt(std::max(sum, 1e-12)));
        for (int k = 0; k < lines; ++k) fromScattered_[static_cast<std::size_t>(k)][static_cast<std::size_t>(v)] *= inv;
    }
}

/*  Takes up a layout. Same line count and something already playing: the other network is built to
 *  it, empty, and the two fade past each other. A fade already running holds the newest layout back
 *  rather than cutting the one in flight. */
void ReverbEngine::adoptPlan(const TailPlan& plan) noexcept {
    const bool fresh = !played_ || !configured_;
    if (fresh || plan.lines != tail_[static_cast<std::size_t>(live_)].lines()) {
        fading_ = -1;
        havePending_ = false;
        tail_[static_cast<std::size_t>(live_)].configure(plan);
        tail_[static_cast<std::size_t>(1 - live_)].reset();
        return;
    }
    if (fading_ >= 0) {
        pending_ = plan;
        havePending_ = true;
        return;
    }
    //  balance_ still holds the room that is leaving: set() works the new one out below this.
    leavingTailGain_ = balance_.tailGain;
    const int next = 1 - live_;
    tail_[static_cast<std::size_t>(next)].configure(plan);
    fading_ = live_;
    live_ = next;
    fadeAt_ = 0;
    havePending_ = false;
}

void ReverbEngine::setRegions(const ReverbFrame& s) noexcept {
    if (!regionsSet_ || !(s.send == sendNow_) || !(s.returnRegion == returnNow_)) {
        const auto virtuals = virtualSourceDirections();
        for (int v = 0; v < kVirtualSources; ++v)
            sendSourceRaw_[static_cast<std::size_t>(v)] =
                static_cast<float>(valueAt(s.send, virtuals[static_cast<std::size_t>(v)]));
        returnPlain_ = !returnOp_.set(s.returnRegion) || returnOp_.passesEverything();
        for (int k = 0; k < lines_; ++k) {
            const auto ki = static_cast<std::size_t>(k);
            sendLineRaw_[ki] = static_cast<float>(valueAt(s.send, lineDirection_[ki]));
            returnLineRaw_[ki] = returnPlain_ ? 1.0f : static_cast<float>(valueAt(s.returnRegion, lineDirection_[ki]));
        }
        sendNow_ = s.send;
        returnNow_ = s.returnRegion;
        regionsSet_ = true;
    }

    const double w = rampWeight(sinceSet_, kReverbHop);
    const auto sendAmount = static_cast<float>(std::max(0.0, s.sendAmount));
    const auto returnAmount = static_cast<float>(std::max(0.0, s.returnAmount));
    std::array<float, kVirtualSources> sourceGain{};
    for (int v = 0; v < kVirtualSources; ++v)
        sourceGain[static_cast<std::size_t>(v)] = sendSourceRaw_[static_cast<std::size_t>(v)] * sendAmount;
    early_.setSourceGains(sourceGain, kReverbHop);
    for (int k = 0; k < lines_; ++k) {
        const auto ki = static_cast<std::size_t>(k);
        const float sendTo = sendLineRaw_[ki] * sendAmount, returnTo = returnLineRaw_[ki] * returnAmount;
        if (snap_) {
            sendLine_[ki].jumpTo(sendTo);
            returnLine_[ki].jumpTo(returnTo);
        } else {
            sendLine_[ki].glideTo(sendTo, static_cast<float>(w));
            returnLine_[ki].glideTo(returnTo, static_cast<float>(w));
        }
    }
}

void ReverbEngine::set(const ReverbFrame& s) noexcept {
    //  A layout held back while the last fade ran, taken up now that it has finished.
    if (fading_ < 0 && havePending_) adoptPlan(pending_);
    //  Distance alone does not rebuild the geometry: recomputing every image of every virtual source
    //  is too costly to pay on every control step for a sub-decibel move in the wet. The geometry is
    //  built with whatever distance is in force when the room is built; distance then moves the dry alone.
    const bool roomMoved = !configured_ || !sameRoom(s.room, settings_.room);
    const bool linesMoved = !configured_ || s.tailLines != settings_.tailLines;
    const bool tailOrderMoved = !configured_ || s.tailOrder != settings_.tailOrder;
    settings_ = s;

    if (roomMoved) {
        room_ = deriveRoom(s.room);
        builtDistance_ = std::max(0.1, s.distance);
        early_.setRoom(room_, builtDistance_);
        //  Lines follow the room's size, the diffuser its mixing time: either moving rebuilds the network.
        if (!configured_ || linesMoved || std::abs(room_.lineScale - planScale_) > 0.02 ||
            room_.mixingTime != planMixing_) {
            const TailPlan plan = planTail(s.tailLines, fs_, room_.lineScale, room_.mixingTime);
            planScale_ = room_.lineScale;
            planMixing_ = room_.mixingTime;
            if (linesMoved || plan.lines != lines_) layOutLines(plan.lines);
            adoptPlan(plan);
        }
    } else if (linesMoved) {
        const TailPlan plan = planTail(s.tailLines, fs_, room_.lineScale, room_.mixingTime);
        planScale_ = room_.lineScale;
        planMixing_ = room_.mixingTime;
        layOutLines(plan.lines);
        adoptPlan(plan);
    }
    if (tailOrderMoved || linesMoved || !configured_) {
        const int order = std::clamp(s.tailOrder, 0, std::min(busOrder_, kEarlyOrder));
        tailChannels_ = numChannels(order);
        std::array<double, kEarlyChannels> y{};
        for (int k = 0; k < lines_; ++k) {
            shSN3D(lineDirection_[static_cast<std::size_t>(k)], order, y);
            for (int c = 0; c < kEarlyChannels; ++c)
                place_[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)] =
                    c < tailChannels_ ? static_cast<float>(y[static_cast<std::size_t>(c)]) : 0.0f;
        }
    }

    // ---- the tail as the room asks for it ------------------------------------------------------------
    //  The source reaches the tail only when the discrete reflections are giving way: at 0.7 of the
    //  mixing time, never before the first wall's reflection, and at most 60 ms.
    balance_.preDelaySeconds = std::clamp(std::max(early_.firstWallSeconds(), 0.7 * room_.mixingTime), 0.0, 0.06);
    TailSettings t;
    t.rtLow = room_.rtLow;
    t.rtMid = room_.rtMid;
    t.rtHigh = room_.rtHigh;
    t.lowCutHz = s.lowCutHz;
    t.highCutHz = s.highCutHz;
    t.preDelaySeconds = std::clamp(balance_.preDelaySeconds + s.preDelayTrimSeconds, 0.0, 0.24);
    t.diffusion = room_.diffusion;
    t.drift = s.drift;
    for (FdnTail& tail : tail_) tail.set(t);

    // ---- one decay ----------------------------------------------------------------------------------
    const double tm = room_.mixingTime, rt = room_.rtMid;
    double K = 1.0;
    if (early_.lateWindowEnergy() > 0.0)
        K = early_.lateWindowEnergy() / (tm * 0.5) * (rt / 13.82) * std::exp(13.82 * 0.75 * tm / rt);
    else if (early_.windowEnergy() > 0.0)
        K = early_.windowEnergy() / (1.0 - std::exp(-13.82 * tm / rt));
    balance_.roomEnergy = K;
    balance_.mirrorEnergy = early_.mirrorEnergy();
    balance_.tailEnergy = std::max(K - balance_.mirrorEnergy, 0.0);
    // what the tail makes of what it is fed: the source, and everything the reflections scattered
    const double a = std::clamp(room_.diffusion, 0.0, 1.0),
                 heardAs = kBalanceAsHeard ? (1.0 - a) * (1.0 - a) + a * a : 1.0;
    const double fed =
        tail_[static_cast<std::size_t>(live_)].energyForUnitImpulse() / heardAs * (1.0 + early_.scatteredEnergy());
    const double norm = 1.0 / std::sqrt(std::max(K, 1e-12)), level = std::pow(10.0, s.levelDb / 20.0);
    balance_.earlyGain = norm;
    balance_.tailGain = norm * std::sqrt(balance_.tailEnergy / std::max(fed, 1e-12));

    setRegions(s);

    leavingRatio_ = fading_ >= 0 ? leavingTailGain_ / std::max(balance_.tailGain, 1e-12) : 1.0;

    const double w = rampWeight(sinceSet_, kReverbHop);
    double earlyTo = balance_.earlyGain * level * std::max(0.0, s.returnAmount);
    double tailTo = balance_.tailGain * level;
    /*  The dry sits at the physically correct direct-to-reverberant ratio, and `dryGain` trims it. K
     *  is the room's energy against a direct sound of 1, so 1/sqrt(K) is the direct sound the
     *  normalised room should be heard against; away from the distance the geometry was built at, the
     *  direct falls as 1/r while the room does not. */
    const double ratio = 1.0 / std::sqrt(std::max(balance_.roomEnergy, 1e-12));
    const double direct = ratio * (builtDistance_ / std::max(0.1, s.distance));
    double dryTo = std::max(0.0, s.dryGain) * direct;
    //  The direct is never louder than what came in: close to the source the ratio above can be very
    //  large, so if the dry would exceed unity everything is scaled down together, keeping the
    //  direct-to-reverberant ratio while staying in range.
    const double headroom = std::max(1.0, dryTo);
    dryTo /= headroom;
    earlyTo /= headroom;
    tailTo /= headroom;
    balance_.dryGain = dryTo;
    const auto retarget = [&](Ramp<double>& r, double target) {
        if (snap_)
            r.jumpTo(target);
        else
            r.glideTo(target, w);
    };
    retarget(earlyGain_, earlyTo);
    retarget(tailGain_, tailTo);
    retarget(dryGain_, dryTo);
    snap_ = false;
    sinceSet_ = 0;
    configured_ = true;
}

void ReverbEngine::process(const float* in, float* out, int frames) noexcept {
    if (frames > 0) played_ = true;
    const int C = channels_, K = lines_, T = std::min(tailChannels_, C);
    for (int done = 0; done < frames;) {
        //  Gains are worked out a frame at a time from an absolute position, so a chunk may straddle
        //  the end of a hop.
        const int n = std::min(kChunk, frames - done);
        const float* src = in + static_cast<std::size_t>(done) * C;
        for (int i = 0; i < n * C; ++i) {
            const float v = src[i];
            clean_[static_cast<std::size_t>(i)] = (v == v && std::abs(v) < 1.0e6f) ? v : 0.0f;
        }
        early_.process(clean_.data(), field_.data(), scattered_.data(), n);
        //  The reflections go back over the whole bus, so their half of the return is the projection.
        const float* reflected = field_.data();
        if (!returnPlain_) {
            returnOp_.apply(field_.data(), placed_.data(), n);
            reflected = placed_.data();
        }

        for (int f = 0; f < n; ++f) {
            const float* x = clean_.data() + f * C;
            const double ws = rampWeight(sinceSet_ + f + 1, kReverbHop);
            const float* sc = scattered_.data() + f * kVirtualSources;
            for (int k = 0; k < K; ++k) {
                const auto ki = static_cast<std::size_t>(k);
                float heard = 0.0f;
                for (int c = 0; c < std::min(C, 4); ++c) heard += read_[ki][static_cast<std::size_t>(c)] * x[c];
                float scat = 0.0f;
                for (int v = 0; v < kVirtualSources; ++v)
                    scat += fromScattered_[ki][static_cast<std::size_t>(v)] * sc[v];
                lineIn_[static_cast<std::size_t>(f * K + k)] = heard * static_cast<float>(sendLine_[ki].at(ws));
                lineScattered_[static_cast<std::size_t>(f * K + k)] = scat;
            }
        }
        tail_[static_cast<std::size_t>(live_)].process(lineIn_.data(), lineScattered_.data(), lineOut_.data(), n);
        if (fading_ >= 0) {
            //  The room it was, fed the same input, on its way out. The two are uncorrelated, so it is
            //  their energies that add and the fade is equal power -- a linear one would dip by 3 dB.
            tail_[static_cast<std::size_t>(fading_)].process(lineIn_.data(), lineScattered_.data(), lineOutOld_.data(),
                                                             n);
            for (int f = 0; f < n; ++f) {
                const double w = std::min(1.0, (fadeAt_ + f + 1) / static_cast<double>(fadeFrames_));
                const auto up = static_cast<float>(std::sqrt(w)),
                           down = static_cast<float>(std::sqrt(1.0 - w) * leavingRatio_);
                for (int k = 0; k < K; ++k) {
                    const auto at = static_cast<std::size_t>(f * K + k);
                    lineOut_[at] = up * lineOut_[at] + down * lineOutOld_[at];
                }
            }
            //  The old network's gain is exactly zero once the fade completes, so letting go of it a
            //  chunk late changes no sample.
            fadeAt_ += n;
            if (fadeAt_ >= fadeFrames_) fading_ = -1;
        }

        float* dst = out + static_cast<std::size_t>(done) * C;
        for (int f = 0; f < n; ++f) {
            const double w = rampWeight(sinceSet_ + f + 1, kReverbHop);
            const auto eg = static_cast<float>(earlyGain_.at(w)), tg = static_cast<float>(tailGain_.at(w));
            float* y = dst + f * C;
            const float* e = reflected + f * C;
            for (int c = 0; c < C; ++c) y[c] = eg * e[c];
            //  The dry, from the guarded input so nothing that is not a number reaches the output here either.
            if (!dryGain_.holds(0.0)) {
                const auto dg = static_cast<float>(dryGain_.at(w));
                const float* x = clean_.data() + f * C;
                for (int c = 0; c < C; ++c) y[c] += dg * x[c];
            }
            const float* lo = lineOut_.data() + f * K;
            for (int k = 0; k < K; ++k) {
                const auto ki = static_cast<std::size_t>(k);
                const float v = tg * lo[k] * static_cast<float>(returnLine_[ki].at(w));
                const float* p = place_[ki].data();
                for (int c = 0; c < T; ++c) y[c] += p[c] * v;
            }
        }
        sinceSet_ = std::min(kReverbHop, sinceSet_ + n);
        done += n;
    }
}

}  // namespace bambi
