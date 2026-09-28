// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>
#include <vector>

#include "bambi/dsp/fft.hpp"

namespace bambi {

/*  The audio feature set — Level, Attack, Tonal, Low, Mid, High.
 *
 *  Chosen for interpretability over information density: a feature the user cannot predict
 *  is worse than no feature. That is why spectral centroid and crest factor are absent
 *  despite encoding more.
 *
 *  Every constant here is calibrated against real material (the Ableton factory library),
 *  not derived from theory, and is deliberately not a user control.
 *
 *    - Attack is a fast/slow envelope ratio, so it is level-independent by construction and
 *      reads zero on anything steady. Spectral flux fires continuously on noise, which is
 *      why it is not used instead.
 *    - Tonal's range is -2..-13 dB of flatness, from where real material actually sits
 *      (-15.4..-1.5 dB, drums -3.9, tonal -10.6). Taking it from where a pure sine sits would
 *      map nearly everything real to zero.
 *
 *  All outputs are unipolar [0,1]. Features are measurements; signed behaviour is a routing
 *  choice made downstream, not a property invented inside a measurement.
 */

enum class Feature { Level, Attack, Tonal, Low, Mid, High };
inline constexpr int kNumFeatures = 6;

struct FeatureConfig {
    double gateDb{-55.0};
    double gateWidthDb{3.0};  ///< the gate opens over gateDb +/- half of this, not at it

    double levelAttackS{0.010}, levelReleaseS{0.200};
    double levelLoDb{-55.0}, levelHiDb{-6.0};

    double attackFastAttackS{0.003}, attackFastReleaseS{0.012};
    double attackSlowAttackS{0.150}, attackSlowReleaseS{0.300};
    double attackLo{1.0}, attackHi{2.2};
    double attackGateFloor{3e-4};

    double tonalLoHz{100.0}, tonalHiHz{8000.0};
    double tonalSmoothS{0.040};
    double tonalLoDb{-2.0}, tonalHiDb{-13.0};

    double bandSplitLoHz{250.0}, bandSplitHiHz{2000.0};
    double bandLoDb{-55.0}, bandHiDb{-12.0};
};

inline constexpr double kLevelReleaseMinS = 0.020, kLevelReleaseMaxS = 2.0;

/*  The power a field carries, a sample: the sum of its channels' squares over (order + 1). SN3D's
 *  harmonics of one degree square-sum to one in every direction, so a single source reads exactly
 *  what W alone reads, and nothing in it can cancel. `channels` are planar, as a host hands them
 *  over, and are whole orders -- (order + 1) is taken as the root of their count, so a caller with
 *  a wider bus than its order passes the first (order + 1)^2 and no more. Real-time safe. */
void fieldPower(std::span<const float* const> channels, int numSamples, float* out) noexcept;

class FeatureBank {
public:
    /// Allocates. fftSize must be a power of two; hop sets the control rate.
    void prepare(double sampleRate, int fftSize = 2048, int hop = 256, const FeatureConfig& cfg = {});

    /// Feed mono audio. Analysis fires at hop boundaries; between them values hold.
    void process(const float* in, int numSamples) { process(in, nullptr, numSamples); }

    /*  The same, with the power given beside the signal: what Level, Attack and the gate read in
     *  place of `in` squared. A field effect's detector hears W for its spectrum and `fieldPower`
     *  for its level, because W alone can cancel. Null is the plain form. Real-time safe. */
    void process(const float* in, const float* power, int numSamples);

    /*  How long Level takes to fall, in seconds: the release of whatever Level drives, ducking
     *  first. Clamped to kLevelReleaseMinS..kLevelReleaseMaxS. Real-time safe; takes
     *  effect at the next hop. */
    void setLevelRelease(double seconds) noexcept;
    double levelRelease() const noexcept { return cfg_.levelReleaseS; }

    double value(Feature f) const { return values_[static_cast<std::size_t>(f)]; }
    std::span<const double> values() const { return values_; }
    double gate() const { return gate_; }

    /*  Only Tonal and the bands need the FFT. Turning it off when nothing routes to them is
     *  the difference that shows up in a real session — 0.064% of a core per instance
     *  instead of 0.136% — far more than optimising any single detector.
     */
    void setSpectralEnabled(bool on) { spectral_ = on; }

    /// Analyse one whole buffer offline, returning per-hop values. For fixtures and tests.
    static std::vector<std::vector<double>> analyseOffline(std::span<const float> mono, double sampleRate,
                                                           int fftSize = 2048, int hop = 256,
                                                           const FeatureConfig& cfg = {});
    void reset();

private:
    void analyseFrame();

    FeatureConfig cfg_;
    double sampleRate_{48000.0};
    int fftSize_{2048}, hop_{256};
    bool spectral_{true};

    std::vector<float> ring_, powerRing_;
    bool usePower_{false};
    std::size_t writePos_{0}, sinceHop_{0}, mask_{0};
    std::vector<double> window_, frame_, mag_;  // mag_ holds power, not magnitude
    RealFft fft_;

    std::array<double, kNumFeatures> values_{};
    double gate_{0.0};
    double envLevel_{0.0}, envTonal_{0.0}, fast_{0.0}, slow_{0.0}, gateEnv_{0.0};
};

}  // namespace bambi
