// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/dsp/features.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/dsp/fft.hpp"
#include "bambi/math/vec3.hpp"

namespace bambi {
namespace {

/// One-pole coefficient. The argument is SECONDS — named so, because writing a time
/// constant in milliseconds into a function that divides dt by it costs a factor of 1000.
double smoothCoef(double dt, double tauSeconds) { return 1.0 - std::exp(-dt / std::max(tauSeconds, 1e-5)); }

double normRange(double v, double lo, double hi) { return clampd((v - lo) / (hi - lo), 0.0, 1.0); }

}  // namespace

void FeatureBank::prepare(double sampleRate, int fftSize, int hop, const FeatureConfig& cfg) {
    cfg_ = cfg;
    sampleRate_ = sampleRate;
    /*  Round up to a power of two. pffft wants a friendly size anyway, and it lets the
     *  ring buffer index with a mask -- an integer division per sample, on the one loop
     *  that sees every sample, is not a price worth paying for arbitrary sizes. */
    fftSize_ = 64;
    while (fftSize_ < fftSize) fftSize_ <<= 1;
    hop_ = std::clamp(hop, 1, fftSize_);
    ring_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
    powerRing_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
    window_.assign(static_cast<std::size_t>(fftSize_), 0.0);
    frame_.assign(static_cast<std::size_t>(fftSize_), 0.0);
    mag_.assign(static_cast<std::size_t>(fftSize_ / 2 + 1), 0.0);
    mask_ = ring_.size() - 1;
    fft_.prepare(static_cast<std::size_t>(fftSize_));
    hannWindow(window_);
    reset();
}

void FeatureBank::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(powerRing_.begin(), powerRing_.end(), 0.0f);
    writePos_ = 0;
    sinceHop_ = 0;
    values_.fill(0.0);
    gate_ = envLevel_ = envTonal_ = fast_ = slow_ = gateEnv_ = 0.0;
}

void fieldPower(std::span<const float* const> channels, int numSamples, float* out) noexcept {
    const auto n = static_cast<std::size_t>(std::max(0, numSamples));
    std::fill_n(out, n, 0.0f);
    if (channels.empty()) return;
    for (const float* x : channels)
        for (std::size_t i = 0; i < n; ++i) out[i] += x[i] * x[i];
    const float k = 1.0f / std::sqrt(static_cast<float>(channels.size()));
    for (std::size_t i = 0; i < n; ++i) out[i] *= k;
}

void FeatureBank::setLevelRelease(double seconds) noexcept {
    if (!std::isfinite(seconds)) return;  // clampd passes a NaN through
    cfg_.levelReleaseS = clampd(seconds, kLevelReleaseMinS, kLevelReleaseMaxS);
}

void FeatureBank::process(const float* in, const float* power, int numSamples) {
    usePower_ = power != nullptr;
    //  Written as runs rather than a per-sample loop: the buffer is copied in at most two
    //  contiguous pieces per hop, and the hop test is made once per run instead of once
    //  per sample.
    std::size_t i = 0;
    const auto n = static_cast<std::size_t>(numSamples);
    while (i < n) {
        const std::size_t toHop = static_cast<std::size_t>(hop_) - sinceHop_;
        const std::size_t run = std::min(n - i, toHop);
        std::size_t left = run;
        while (left > 0) {
            const std::size_t chunk = std::min(left, ring_.size() - writePos_);
            std::copy_n(in + i, chunk, ring_.begin() + static_cast<std::ptrdiff_t>(writePos_));
            if (power != nullptr)
                std::copy_n(power + i, chunk, powerRing_.begin() + static_cast<std::ptrdiff_t>(writePos_));
            i += chunk;
            writePos_ = (writePos_ + chunk) & mask_;
            left -= chunk;
        }
        sinceHop_ += run;
        if (sinceHop_ >= static_cast<std::size_t>(hop_)) {
            sinceHop_ = 0;
            analyseFrame();
        }
    }
}

void FeatureBank::analyseFrame() {
    const std::size_t n = ring_.size();
    const double dt = static_cast<double>(hop_) / sampleRate_;

    // Frame RMS from the time domain. STFT magnitudes carry the window's own scaling, and
    // reading absolute level off them lands tens of dB low — the spectrum below is used
    // only for ratios, which are scaling-invariant.
    double acc = 0.0, given = 0.0;
    {
        //  The ring is unwrapped as two contiguous runs and windowed in the same pass, instead
        //  of a modulo per sample followed by a second full pass to apply the window -- at
        //  2048 samples every hop, that second pass would be the largest avoidable cost here.
        std::size_t k = 0;
        for (std::size_t part = 0; part < 2; ++part) {
            const std::size_t start = (part == 0) ? writePos_ : 0;
            const std::size_t count = (part == 0) ? n - writePos_ : writePos_;
            //  the power rides the same pass: a second chain the first does not wait on, so it is free
            for (std::size_t j = 0; j < count; ++j, ++k) {
                const double v = ring_[start + j];
                acc += v * v;
                given += powerRing_[start + j];
                frame_[k] = v * window_[k];
            }
        }
    }
    /*  With a power given, that is the level: the signal's own square can be nothing while the field
     *  is loud. The spectrum below still comes from the signal -- it is ratios, and needs a wave. */
    double rms = std::sqrt((usePower_ ? given : acc) / static_cast<double>(n)) + 1e-12;
    /*  One non-finite sample, in any channel, would otherwise be in every follower for good: they
     *  are one-poles, and a NaN never leaves one. Read as silence, it is gone when the ring has
     *  turned over -- 43 ms -- instead of at the next reset. */
    if (!std::isfinite(rms)) rms = 1e-12;
    const double db = 20.0 * std::log10(rms);

    /*  The gate. Without it silence reads as a flat spectrum (percussive) and as constant
     *  per-band content (tonal) — both artefacts of dividing by nothing — and the plugin's
     *  premise is that silence parks the source. It opens over a band, as Attack does: a level
     *  that two machines round differently at the threshold then moves the gate by as much, not
     *  by all of it. */
    const double half = 0.5 * cfg_.gateWidthDb;
    gateEnv_ += (normRange(db, cfg_.gateDb - half, cfg_.gateDb + half) - gateEnv_) * smoothCoef(dt, 0.010);
    gate_ = clampd(gateEnv_ * 1.4, 0.0, 1.0);

    envLevel_ += (rms - envLevel_) * smoothCoef(dt, rms > envLevel_ ? cfg_.levelAttackS : cfg_.levelReleaseS);
    const double envDb = 20.0 * std::log10(std::max(envLevel_, 1e-12));
    /*  Not multiplied by the gate, as the other five are. Level needs no protecting from
     *  silence: its envelope falls to nothing on its own, and its range ends where the gate
     *  opens. With the gate, the fall at silence took 15 ms whatever the release said -- and a
     *  ducker's wet came back at once when a clip ended. */
    values_[static_cast<std::size_t>(Feature::Level)] = normRange(envDb, cfg_.levelLoDb, cfg_.levelHiDb);

    fast_ += (rms - fast_) * smoothCoef(dt, rms > fast_ ? cfg_.attackFastAttackS : cfg_.attackFastReleaseS);
    slow_ += (rms - slow_) * smoothCoef(dt, rms > slow_ ? cfg_.attackSlowAttackS : cfg_.attackSlowReleaseS);
    /*  Opening out of silence is a weight, not a decision. A hard `slow_ > floor` branch would step
     *  the Attack value from 0 to its full swing in one hop, right at the transient, with gate_
     *  already ~0.58 -- so one float32 ULP of difference in slow_ (which a different architecture,
     *  or a differently chosen FMA, can produce) would move a whole hop of gain, and a rate
     *  destination (which modulation does not smooth, because the motion clock integrates it)
     *  would turn that into a permanent position offset. A ramp makes the value continuous in the
     *  envelope, so a ULP in gives a ULP out.
     *
     *  The band is attackHi/attackLo wide -- the same relative interval the ratio itself spans --
     *  so the open weight is never the steepest term in the product. It introduces no new number.
     *  Hysteresis would not do this: a latch is still a step, one hop later, and it would make
     *  Attack depend on history, so a bounce from bar 5 would read differently from realtime
     *  through bar 5. A latch belongs where the output is an event (EnvTrigger); a ramp belongs
     *  where the output is a measurement. */
    const double openHi = cfg_.attackGateFloor * (cfg_.attackHi / std::max(cfg_.attackLo, 1e-9));
    const double openWeight = normRange(slow_, cfg_.attackGateFloor, openHi);
    const double ratio = fast_ / std::max(slow_, cfg_.attackGateFloor);
    values_[static_cast<std::size_t>(Feature::Attack)] =
        normRange(ratio, cfg_.attackLo, cfg_.attackHi) * openWeight * gate_;

    if (!spectral_) {
        values_[static_cast<std::size_t>(Feature::Tonal)] = 0.0;
        values_[static_cast<std::size_t>(Feature::Low)] = 0.0;
        values_[static_cast<std::size_t>(Feature::Mid)] = 0.0;
        values_[static_cast<std::size_t>(Feature::High)] = 0.0;
        return;
    }

    fft_.power(frame_, mag_);  // power, not magnitude: see the band loop below

    const std::size_t bins = mag_.size();
    const double nyq = sampleRate_ * 0.5;
    const auto binOf = [&](double hz) {
        return static_cast<std::size_t>(
            clampd(hz / nyq * static_cast<double>(bins - 1), 0.0, static_cast<double>(bins - 1)));
    };

    // Tonal: spectral flatness, geometric over arithmetic mean.
    const std::size_t i0 = std::max<std::size_t>(1, binOf(cfg_.tonalLoHz));
    const std::size_t i1 = std::min(bins, binOf(cfg_.tonalHiHz));
    /*  Flatness is the geometric over the arithmetic mean of magnitude. Two things here:
     *
     *  mag_ holds power, so the square root is taken only across the tonal band rather
     *  than the whole spectrum -- the other ~700 bins never need one.
     *
     *  The geometric mean is accumulated as a product, not as a sum of logarithms: a sum of
     *  logs is one transcendental call per bin, the single largest non-FFT cost in this
     *  function, where log of a product is one call for the whole band. The running product
     *  would underflow after roughly a hundred bins, so its exponent is carried separately in
     *  an integer. Agreement with the per-bin sum is 2e-16, exact to the last bit that matters.
     *
     *  This stays magnitude flatness on purpose: power flatness is a different number, and the
     *  -2..-13 dB range is calibrated against this one. */
    double prod = 1.0, linSum = 0.0;
    int prodExp = 0;
    std::size_t cnt = 0;
    for (std::size_t i = i0; i < i1; ++i) {
        const double m = std::sqrt(std::max(mag_[i], 1e-20));
        linSum += m;
        prod *= m;
        //  Cheap, almost never taken, and the reason the product cannot underflow: pull the
        //  exponent out into an integer whenever the running product gets small.
        if (prod < 1e-100) {
            int e = 0;
            prod = std::frexp(prod, &e);
            prodExp += e;
        }
        ++cnt;
    }
    const double logSum = std::log(prod) + prodExp * 0.6931471805599453;
    //  The fallback is the not-tonal end of the range, which runs downward (-2 to -13).
    double flatDb = cfg_.tonalLoDb;
    if (cnt > 0 && linSum > 1e-12) {
        const double flat = std::exp(logSum / static_cast<double>(cnt)) / (linSum / static_cast<double>(cnt));
        flatDb = 10.0 * std::log10(std::max(flat, 1e-8));
    }
    const double tRaw = normRange(flatDb, cfg_.tonalLoDb, cfg_.tonalHiDb);
    envTonal_ += (tRaw - envTonal_) * smoothCoef(dt, cfg_.tonalSmoothS);
    values_[static_cast<std::size_t>(Feature::Tonal)] = clampd(envTonal_, 0.0, 1.0) * gate_;

    /*  Bands by Parseval: take each band's fraction of spectral energy and scale the true
     *  time-domain level by it, so band levels are calibrated exactly like Level rather
     *  than in STFT units. */
    const std::size_t b1 = binOf(cfg_.bandSplitLoHz), b2 = binOf(cfg_.bandSplitHiHz);
    double pLo = 0.0, pMid = 0.0, pHi = 0.0;
    for (std::size_t i = 0; i < b1; ++i) pLo += mag_[i];
    for (std::size_t i = b1; i < b2; ++i) pMid += mag_[i];
    for (std::size_t i = b2; i < bins; ++i) pHi += mag_[i];
    const double tot = std::max(pLo + pMid + pHi, 1e-20);
    const auto bandVal = [&](double power) {
        const double e = rms * std::sqrt(power / tot) + 1e-12;
        return normRange(20.0 * std::log10(e), cfg_.bandLoDb, cfg_.bandHiDb) * gate_;
    };
    values_[static_cast<std::size_t>(Feature::Low)] = bandVal(pLo);
    values_[static_cast<std::size_t>(Feature::Mid)] = bandVal(pMid);
    values_[static_cast<std::size_t>(Feature::High)] = bandVal(pHi);
}

std::vector<std::vector<double>> FeatureBank::analyseOffline(std::span<const float> mono, double sampleRate,
                                                             int fftSize, int hop, const FeatureConfig& cfg) {
    FeatureBank fb;
    fb.prepare(sampleRate, fftSize, hop, cfg);
    std::vector<std::vector<double>> out;
    const std::size_t total = mono.size();
    for (std::size_t i = 0; i < total; i += static_cast<std::size_t>(hop)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(hop), total - i));
        fb.process(mono.data() + i, n);
        std::vector<double> row(fb.values().begin(), fb.values().end());
        row.push_back(fb.gate());
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace bambi
