// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/early.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"

namespace bambi {
namespace {

constexpr double kSilent = 1e-5;  // a tap quieter than this is not played
const double kLowpassQ = std::pow(10.0, 0.5 / 20.0);

double classHz(int k) { return 800.0 * std::pow(25.0, k / 9.0); }
int classOf(double hz) {
    return std::clamp(static_cast<int>(std::lround(std::log(hz / 800.0) / std::log(25.0) * 9.0)), 0,
                      kCutoffClasses - 1);
}

std::array<Vec3, 6> sixDirections() {
    std::array<Vec3, 6> out{};
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < 6; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / 6.0, r = std::sqrt(1.0 - z * z), a = i * golden;
        out[static_cast<std::size_t>(i)] = {r * std::cos(a), r * std::sin(a), z};
    }
    return out;
}

//  The energy of a set of (sample, amplitude) taps played as an impulse response: taps on one sample
//  add their amplitudes, taps apart their energies.
double energyOf(std::vector<std::pair<int, double>>& taps) {
    std::sort(taps.begin(), taps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    double energy = 0.0;
    for (std::size_t i = 0; i < taps.size();) {
        double sum = 0.0;
        const int at = taps[i].first;
        for (; i < taps.size() && taps[i].first == at; ++i) sum += taps[i].second;
        energy += sum * sum;
    }
    return energy;
}

}  // namespace

void EarlyReflections::prepare(int busOrder, double sampleRate) {
    busOrder_ = busOrder;
    busChannels_ = numChannels(busOrder);
    used_ = std::min(busChannels_, kEarlyChannels);
    for (auto& g : gain_) g.jumpTo(1.0f);
    hop_ = 1;
    sinceGain_ = 0;
    snapGain_ = true;
    plainGain_ = true;
    fs_ = sampleRate;
    lineSize_ = static_cast<int>(std::ceil(kMaxReflectionSeconds * sampleRate)) + 4;
    for (auto& l : line_) l.assign(static_cast<std::size_t>(lineSize_), 0.0f);
    bus_ = std::make_unique<BusReflections>();
    scratch_.reserve(static_cast<std::size_t>(kVirtualSources * kMaxImages));
    for (auto& k : acc_) k.assign(static_cast<std::size_t>(kEarlyChannels * 64), 0.0f);

    //  Reading the bus at a direction with a max-rE beam: by the addition theorem the beam toward v of
    //  a source at u is the sum over channels of (2n+1) w_n Y(v) Y(u) over the sum of (2n+1) w_n.
    const int order = std::min(busOrder, kEarlyOrder);
    const double at = std::cos(137.9 * kDeg2Rad / (order + 1.51));
    std::array<double, kEarlyOrder + 1> w{};
    double norm = 0.0;
    for (int n = 0; n <= order; ++n) {
        w[static_cast<std::size_t>(n)] = n == 0   ? 1.0
                                         : n == 1 ? at
                                                  : ((2 * n - 1) * at * w[static_cast<std::size_t>(n - 1)] -
                                                     (n - 1) * w[static_cast<std::size_t>(n - 2)]) /
                                                        n;
        norm += (2 * n + 1) * w[static_cast<std::size_t>(n)];
    }
    const auto dirs = virtualSourceDirections();
    std::array<double, kEarlyChannels> y{};
    for (int v = 0; v < kVirtualSources; ++v) {
        shSN3D(dirs[static_cast<std::size_t>(v)], order, y);
        for (int c = 0; c < used_; ++c) {
            const int n = acnOrder(c);
            decode_[static_cast<std::size_t>(v)][static_cast<std::size_t>(c)] = static_cast<float>(
                (2 * n + 1) * w[static_cast<std::size_t>(n)] * y[static_cast<std::size_t>(c)] / norm);
        }
    }
    for (int k = 0; k < kCutoffClasses; ++k)
        lowpass_[static_cast<std::size_t>(k)] = lowPass(classHz(k), kLowpassQ, sampleRate);
    reset();
    setRoom(deriveRoom(RoomSettings{}), 6.0);
}

void EarlyReflections::reset() noexcept {
    for (auto& l : line_) std::fill(l.begin(), l.end(), 0.0f);
    for (auto& k : state_) k = ClassState{};
    write_ = 0;
    snapGain_ = true;
}

void EarlyReflections::setSourceGains(const std::array<float, kVirtualSources>& gains, int hopFrames) noexcept {
    const double w = rampWeight(sinceGain_, hop_);
    bool plain = true;
    for (int v = 0; v < kVirtualSources; ++v) {
        const auto vi = static_cast<std::size_t>(v);
        if (snapGain_)
            gain_[vi].jumpTo(gains[vi]);
        else
            gain_[vi].glideTo(gains[vi], w);
        plain = plain && gain_[vi].holds(1.0f);
    }
    plainGain_ = plain;
    hop_ = std::max(1, hopFrames);
    sinceGain_ = 0;
    snapGain_ = false;
}

void EarlyReflections::setRoom(const Room& room, double distance) noexcept {
    busReflections(room, distance, *bus_);
    const int count = bus_->count, order = std::min(busOrder_, kEarlyOrder);

    // ---- the level: the bus's reflections against one exact source's, for six directions ---------
    double exact = 0.0, onBus = 0.0;
    mirrorEnergy_ = scatteredEnergy_ = lateEnergy_ = windowEnergy_ = firstWall_ = 0.0;
    firstDelay_ = 1e9;
    std::array<Reflection, kMaxImages> one{};
    for (const Vec3& u : sixDirections()) {
        reflectionsOf(room, u, distance, one);
        scratch_.clear();
        for (int i = 0; i < count; ++i) {
            const Reflection& t = one[static_cast<std::size_t>(i)];
            if (t.mirror > kSilent) scratch_.emplace_back(static_cast<int>(std::lround(t.delay * fs_)), t.mirror);
            mirrorEnergy_ += t.mirror * t.mirror / 6.0;
            scatteredEnergy_ += t.scattered * t.scattered / 6.0;
            if (t.delay < room.mixingTime) {
                windowEnergy_ += t.gain * t.gain / 6.0;
                if (t.delay >= room.mixingTime * 0.5) lateEnergy_ += t.gain * t.gain / 6.0;
            }
        }
        double wall = 1e9;
        for (int i = 0; i < count; ++i)
            if (one[static_cast<std::size_t>(i)].order == 1)
                wall = std::min(wall, one[static_cast<std::size_t>(i)].delay);
        firstWall_ += wall / 6.0;
        exact += energyOf(scratch_);

        //  the same source as the bus hears it: at every virtual source, by its beam, weights keeping energy
        std::array<double, kVirtualSources> weight{};
        double sum = 0.0;
        for (int v = 0; v < kVirtualSources; ++v) {
            weight[static_cast<std::size_t>(v)] =
                maxReBeam(order, dot(bus_->direction[static_cast<std::size_t>(v)], u));
            sum += weight[static_cast<std::size_t>(v)] * weight[static_cast<std::size_t>(v)];
        }
        scratch_.clear();
        for (int v = 0; v < kVirtualSources; ++v)
            for (int i = 0; i < count; ++i) {
                const Reflection& t = bus_->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)];
                if (t.mirror > kSilent)
                    scratch_.emplace_back(static_cast<int>(std::lround(t.delay * fs_)),
                                          t.mirror * weight[static_cast<std::size_t>(v)] / std::sqrt(sum));
            }
        onBus += energyOf(scratch_);
    }
    level_ = onBus > 0.0 ? std::sqrt(exact / onBus) : 1.0;

    // ---- the taps ----------------------------------------------------------------------------------
    //  The beams' weights are not normalised a direction at a time on a bus -- nothing knows the
    //  direction -- so the twelve are scaled once, by what that normalisation is on average.
    double meanSquare = 0.0;
    for (const Vec3& u : sixDirections())
        for (int v = 0; v < kVirtualSources; ++v) {
            const double b = maxReBeam(order, dot(bus_->direction[static_cast<std::size_t>(v)], u));
            meanSquare += b * b / 6.0;
        }
    const double scale = level_ / std::sqrt(std::max(meanSquare, 1e-12));

    liveTaps_ = 0;
    narrowedTaps_ = 0;
    double narrowedEnergy = 0.0, allEnergy = 0.0;
    classUsed_.fill(false);
    classWide_.fill(0);
    const int narrowFrom = static_cast<int>(kWideUntilMixingTimes * room.mixingTime * fs_);
    double loudest = 0.0;
    for (int v = 0; v < kVirtualSources; ++v)
        for (int i = 0; i < count; ++i)
            loudest = std::max(loudest, bus_->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)].mirror);
    const auto narrowBelow = static_cast<float>(loudest * scale * std::pow(10.0, kWideAboveDb / 20.0));
    std::array<double, kEarlyChannels> y{};
    for (int v = 0; v < kVirtualSources; ++v) {
        int n = 0;
        for (int i = 0; i < count; ++i) {
            const Reflection& t = bus_->taps[static_cast<std::size_t>(v)][static_cast<std::size_t>(i)];
            if (t.mirror <= kSilent && t.scattered <= kSilent) continue;
            Tap& tap = taps_[static_cast<std::size_t>(v)][static_cast<std::size_t>(n++)];
            tap.delay = std::clamp(static_cast<int>(std::lround(t.delay * fs_)), 1, lineSize_ - 2);
            tap.klass = classOf(t.cutoffHz);
            tap.mirror = t.mirror > kSilent ? static_cast<float>(t.mirror * scale) : 0.0f;
            tap.scattered = static_cast<float>(t.scattered * scale);
            shSN3D(t.direction, order, y);
            for (int c = 0; c < used_; ++c)
                tap.encode[static_cast<std::size_t>(c)] =
                    tap.mirror * static_cast<float>(y[static_cast<std::size_t>(c)]);
            //  late: order 1, so the channels above it are not written at all rather than written as zero
            tap.wide = (tap.delay < narrowFrom || tap.mirror >= narrowBelow) ? used_ : std::min(used_, 4);
            for (int c = tap.wide; c < used_; ++c) tap.encode[static_cast<std::size_t>(c)] = 0.0f;
            if (tap.mirror > 0.0f) {
                ++liveTaps_;
                allEnergy += static_cast<double>(tap.mirror) * tap.mirror;
                if (tap.wide < used_) {
                    ++narrowedTaps_;
                    narrowedEnergy += static_cast<double>(tap.mirror) * tap.mirror;
                }
                classUsed_[static_cast<std::size_t>(tap.klass)] = true;
                classWide_[static_cast<std::size_t>(tap.klass)] =
                    std::max(classWide_[static_cast<std::size_t>(tap.klass)], tap.wide);
                firstDelay_ = std::min(firstDelay_, tap.delay / fs_);
            }
        }
        tapCount_[static_cast<std::size_t>(v)] = n;
    }
    narrowedShare_ = allEnergy > 0.0 ? narrowedEnergy / allEnergy : 0.0;
    if (firstDelay_ > 1e8) firstDelay_ = 0.0;
}

void EarlyReflections::process(const float* in, float* field, float* scattered, int frames) noexcept {
    const int C = busChannels_, U = used_;
    /*  A chunk at a time, and within it a tap at a time rather than a sample at a time: a tap reads a
     *  run of its line and adds it, scaled, to sixteen runs of its class -- contiguous, and the same
     *  arithmetic in the same order for every sample, so how the audio is cut cannot change a bit.
     *  There is no feedback here, so a chunk's input can all be written before any of it is read. */
    constexpr int kChunk = 64;
    static_assert(kChunk <= 64);
    std::array<float, kChunk> heard{}, run{}, tail{};
    for (int done = 0; done < frames;) {
        const int n = std::min(kChunk, frames - done);
        const float* x = in + done * C;
        for (int k = 0; k < kCutoffClasses; ++k) {
            const auto ki = static_cast<std::size_t>(k);
            if (classUsed_[ki]) std::fill(acc_[ki].begin(), acc_[ki].begin() + classWide_[ki] * kChunk, 0.0f);
        }

        for (int v = 0; v < kVirtualSources; ++v) {
            const auto vi = static_cast<std::size_t>(v);
            std::vector<float>& line = line_[vi];
            //  what this virtual source hears of the bus, into its line
            for (int f = 0; f < n; ++f) {
                float sum = 0.0f;
                for (int c = 0; c < U; ++c) sum += decode_[vi][static_cast<std::size_t>(c)] * x[f * C + c];
                heard[static_cast<std::size_t>(f)] = sum;
            }
            if (!plainGain_) {
                const Ramp<float> gain = gain_[vi];
                for (int f = 0; f < n; ++f) {
                    heard[static_cast<std::size_t>(f)] *=
                        static_cast<float>(gain.at(rampWeight(sinceGain_ + f + 1, hop_)));
                }
            }
            for (int f = 0; f < n; ++f)
                line[static_cast<std::size_t>((write_ + f) % lineSize_)] = heard[static_cast<std::size_t>(f)];

            tail.fill(0.0f);  // gathered in a run of its own and put into its interleaved place once
            const int count = tapCount_[vi];
            for (int i = 0; i < count; ++i) {
                const Tap& t = taps_[vi][static_cast<std::size_t>(i)];
                int at = write_ - t.delay;
                if (at < 0) at += lineSize_;
                //  the tap's run of the line: read where it lies, and straightened out only where it wraps
                const float* src = line.data() + at;
                if (at + n > lineSize_) {
                    const int first = lineSize_ - at;
                    std::copy(line.begin() + at, line.end(), run.begin());
                    std::copy(line.begin(), line.begin() + (n - first), run.begin() + first);
                    src = run.data();
                }
                for (int f = 0; f < n; ++f) tail[static_cast<std::size_t>(f)] += t.scattered * src[f];
                if (t.mirror == 0.0f) continue;
                float* a = acc_[static_cast<std::size_t>(t.klass)].data();
                for (int c = 0; c < t.wide; ++c) {
                    const float e = t.encode[static_cast<std::size_t>(c)];
                    float* row = a + c * kChunk;
                    for (int f = 0; f < n; ++f) row[f] += e * src[f];
                }
            }
            for (int f = 0; f < n; ++f) scattered[(done + f) * kVirtualSources + v] = tail[static_cast<std::size_t>(f)];
        }
        write_ = (write_ + n) % lineSize_;
        sinceGain_ = std::min(hop_, sinceGain_ + n);

        //  each class of dullness through its own low-pass, a channel at a time; the brightest is left alone
        float* out = field + done * C;
        std::fill(out, out + n * C, 0.0f);
        for (int k = 0; k < kCutoffClasses; ++k) {
            const auto ki = static_cast<std::size_t>(k);
            if (!classUsed_[ki]) continue;
            const float* a = acc_[ki].data();
            const int W = std::min(classWide_[ki], U);
            if (k == kCutoffClasses - 1) {
                for (int c = 0; c < W; ++c)
                    for (int f = 0; f < n; ++f) out[f * C + c] += a[c * kChunk + f];
                continue;
            }
            //  One design, sixteen channels in step: frame outer and channel inner, so the sixteen
            //  states advance together and the compiler can take them several at a time.
            const BiquadCoeffs q = lowpass_[ki];
            ClassState& st = state_[ki];
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < W; ++c) {
                    const auto ci = static_cast<std::size_t>(c);
                    const double xin = a[c * kChunk + f];
                    const double y =
                        q.b0 * xin + q.b1 * st.x1[ci] + q.b2 * st.x2[ci] - q.a1 * st.y1[ci] - q.a2 * st.y2[ci];
                    st.x2[ci] = st.x1[ci];
                    st.x1[ci] = xin;
                    st.y2[ci] = st.y1[ci];
                    st.y1[ci] = y;
                    out[f * C + c] += static_cast<float>(y);
                }
        }
        done += n;
    }
}

}  // namespace bambi
