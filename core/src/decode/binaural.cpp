// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/binaural.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/legendre.hpp"
#include "bambi/math/lstsq.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
using Cx = std::complex<double>;
constexpr double kSpeedOfSound = 343.0;
constexpr double kDbToAmplitude = 0.11512925464970229;  // ln 10 / 20

std::size_t sz(int n) { return static_cast<std::size_t>(n); }

/// A 2x2 complex matrix [[a, b], [c, d]].
struct M2 {
    Cx a, b, c, d;
};
M2 mul(const M2& x, const M2& y) {
    return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}
M2 adjoint(const M2& x) { return {std::conj(x.a), std::conj(x.c), std::conj(x.b), std::conj(x.d)}; }
M2 inverse(const M2& x) {
    const Cx det = x.a * x.d - x.b * x.c;
    return {x.d / det, -x.b / det, -x.c / det, x.a / det};
}
/// The square root of a Hermitian positive semi-definite matrix: (A + sqrt(det A) I) / sqrt(tr A + 2 sqrt(det A)),
/// its eigenvalues lifted by a trillionth of the trace first so that the root stays invertible where a bin is
/// silent -- relative, since the covariances scale with the head's level and an absolute lift would take over on
/// a quiet head.
M2 sqrtPsd(M2 x) {
    const double lift = 1e-12 * std::real(x.a + x.d) + 1e-300;
    x.a += lift;
    x.d += lift;
    const double s = std::sqrt(std::max(0.0, std::real(x.a * x.d - x.b * x.c)));
    const double t = std::sqrt(std::real(x.a + x.d) + 2.0 * s);
    return {(x.a + s) / t, x.b / t, x.c / t, (x.d + s) / t};
}

/// The unitary part of x^H: V U^H for x = U S V^H, as x^H (x x^H)^-1/2. Scale-free: x is normalised first.
M2 polarOfAdjoint(M2 x) {
    const double scale = std::sqrt(std::norm(x.a) + std::norm(x.b) + std::norm(x.c) + std::norm(x.d)) + 1e-300;
    x = {x.a / scale, x.b / scale, x.c / scale, x.d / scale};
    return mul(adjoint(x), inverse(sqrtPsd(mul(x, adjoint(x)))));
}

}  // namespace

bool BinauralFilterBuilder::prepare(const HeadModel& model, const FilterDesign& design,
                                    std::span<const Vec3> directions, std::span<const double> weights) {
    design_ = design;
    ready_ = fft_.prepare(sz(std::max(design.fftSize, 0))) && design.taps > 0 && design.taps <= design.fftSize &&
             design.order >= 0 && design.order <= kMaxOrder && !directions.empty() &&
             directions.size() == weights.size();
    if (!ready_) {
        filters_.clear();
        return false;
    }
    directions_ = static_cast<int>(directions.size());
    channels_ = numChannels(design.order);
    cutoff_ = design.order * kSpeedOfSound / (2.0 * kPi * design.headRadius);
    weight_.assign(weights.begin(), weights.end());

    //  The head on the design's bins: the ERB number of each bin's frequency.
    grid_.assign(directions.begin(), directions.end());
    binErbs_.resize(sz(bins()));
    for (int k = 0; k < bins(); ++k) binErbs_[sz(k)] = 21.4 * std::log10(1.0 + 0.00437 * binHz(k));
    head_.prepare(model, grid_, binErbs_);

    basis_.resize(sz(directions_) * sz(channels_));
    for (int d = 0; d < directions_; ++d)
        shSN3D(directions[sz(d)], design.order,
               std::span<double>(basis_.data() + sz(d) * sz(channels_), sz(channels_)));

    //  The weighted pseudo-inverse (Y^T W Y)^-1 Y^T W, channels x directions.
    std::vector<double> gram;
    gramMatrix(basis_, weight_, channels_, gram);
    invertSpd(gram, channels_);
    pinv_.assign(sz(channels_) * sz(directions_), 0.0);
    for (int c = 0; c < channels_; ++c)
        for (int d = 0; d < directions_; ++d) {
            double s = 0;
            for (int i = 0; i < channels_; ++i)
                s += gram[sz(c) * sz(channels_) + sz(i)] * basis_[sz(d) * sz(channels_) + sz(i)];
            pinv_[sz(c) * sz(directions_) + sz(d)] = s * weight_[sz(d)];
        }

    //  A diffuse field of unit power carries 1 / (2n + 1) per SN3D channel of order n.
    diffuse_.resize(sz(channels_));
    for (int c = 0; c < channels_; ++c) diffuse_[sz(c)] = 1.0 / (2.0 * acnOrder(c) + 1.0);

    window_.assign(sz(design.taps), 1.0);
    for (int i = 0; i < design.fade && i < design.taps; ++i)
        window_[sz(design.taps - design.fade + i)] = 0.5 + 0.5 * std::cos(kPi * i / std::max(1, design.fade - 1));

    amplitude_.resize(sz(bins()) * sz(directions_) * 2);
    target_.resize(sz(directions_) * 2);
    carried_.resize(sz(directions_) * 2);
    decode_.resize(sz(bins()) * sz(channels_) * 2);
    re_.resize(sz(bins()));
    im_.resize(sz(bins()));
    ir_.resize(sz(design.fftSize));
    filters_.assign(sz(channels_) * 2 * sz(design.taps), 0.0f);
    return true;
}

void BinauralFilterBuilder::setModel(const HeadModel& model) {
    if (ready_) head_.prepare(model, grid_, binErbs_);
}

void BinauralFilterBuilder::decodedEars(int k, std::span<Cx> out) const {
    const Cx* b = decode_.data() + sz(k) * sz(channels_) * 2;
    for (int d = 0; d < directions_; ++d) {
        const double* y = basis_.data() + sz(d) * sz(channels_);
        Cx left = 0, right = 0;
        for (int c = 0; c < channels_; ++c) {
            left += y[c] * b[2 * c];
            right += y[c] * b[2 * c + 1];
        }
        out[sz(d) * 2] = left;
        out[sz(d) * 2 + 1] = right;
    }
}

void BinauralFilterBuilder::build(double earShiftErb, double headSize) {
    if (!ready_) return;
    head_.synthesise(earShiftErb, headSize);
    const int D = directions_, C = channels_, K = bins();

    //  The head as complex HRTFs: the synthesised magnitudes with a pure interaural delay, half the ITD on each ear,
    //  the left later when the ITD is positive. The fit takes that phase up to the cutoff; above it only the
    //  magnitudes are the head's and the phase is the previous bin's decode, so the filters stay smooth and the
    //  order is not asked to hold phase it cannot.
    for (int k = 0; k < K; ++k) {
        const double omega = 2.0 * kPi * binHz(k);
        const bool phaseFit = k == 0 || binHz(k) <= cutoff_;
        double* amp = amplitude_.data() + sz(k) * sz(D) * 2;
        for (int d = 0; d < D; ++d) {
            const double left = std::exp(head_.left(d)[sz(k)] * kDbToAmplitude);
            const double right = std::exp(head_.right(d)[sz(k)] * kDbToAmplitude);
            amp[2 * d] = left;
            amp[2 * d + 1] = right;
            if (phaseFit) {
                const Cx lag = std::polar(1.0, -0.5e-6 * head_.itd(d) * omega);
                target_[sz(d) * 2] = left * lag;
                target_[sz(d) * 2 + 1] = right * std::conj(lag);
            } else {
                //  The carried phase as a unit phasor, without the angle itself.
                const Cx cl = carried_[sz(d) * 2], cr = carried_[sz(d) * 2 + 1];
                target_[sz(d) * 2] = left * (cl / (std::abs(cl) + 1e-300));
                target_[sz(d) * 2 + 1] = right * (cr / (std::abs(cr) + 1e-300));
            }
        }
        Cx* b = decode_.data() + sz(k) * sz(C) * 2;
        for (int c = 0; c < C; ++c) {
            const double* p = pinv_.data() + sz(c) * sz(D);
            Cx left = 0, right = 0;
            for (int d = 0; d < D; ++d) {
                left += p[d] * target_[sz(d) * 2];
                right += p[d] * target_[sz(d) * 2 + 1];
            }
            b[2 * c] = left;
            b[2 * c + 1] = right;
        }
        if (k + 1 < K && binHz(k + 1) > cutoff_) decodedEars(k, carried_);
    }

    //  The covariance constraint: at every bin the decode's diffuse-field ear covariance is made the head's, with
    //  the mix that changes the signals least.
    for (int k = 0; k < K; ++k) {
        const double omega = 2.0 * kPi * binHz(k);
        const double* amp = amplitude_.data() + sz(k) * sz(D) * 2;
        M2 target{0, 0, 0, 0}, decoded{0, 0, 0, 0};
        for (int d = 0; d < D; ++d) {
            const double w = weight_[sz(d)], left = amp[2 * d], right = amp[2 * d + 1];
            target.a += w * left * left;
            target.d += w * right * right;
            target.b += w * left * right * std::polar(1.0, -1e-6 * head_.itd(d) * omega);
        }
        target.c = std::conj(target.b);
        Cx* b = decode_.data() + sz(k) * sz(C) * 2;
        for (int c = 0; c < C; ++c) {
            const double dn = diffuse_[sz(c)];
            decoded.a += dn * std::norm(b[2 * c]);
            decoded.d += dn * std::norm(b[2 * c + 1]);
            decoded.b += dn * b[2 * c] * std::conj(b[2 * c + 1]);
        }
        decoded.c = std::conj(decoded.b);
        const M2 kx = sqrtPsd(decoded), ky = sqrtPsd(target);
        const M2 x = mul(adjoint(kx), ky);
        const M2 m = mul(mul(ky, polarOfAdjoint(x)), inverse(kx));
        for (int c = 0; c < C; ++c) {
            const Cx left = b[2 * c], right = b[2 * c + 1];
            b[2 * c] = left * m.a + right * m.b;
            b[2 * c + 1] = left * m.c + right * m.d;
        }
    }

    //  Back to time: each spectrum inverted, turned by the bulk delay (a whole number of samples, so a circular
    //  shift), cut to the taps and faded out.
    const int N = design_.fftSize;
    for (int c = 0; c < C; ++c)
        for (int e = 0; e < 2; ++e) {
            for (int k = 0; k < K; ++k) {
                const Cx v = decode_[(sz(k) * sz(C) + sz(c)) * 2 + sz(e)];
                re_[sz(k)] = v.real();
                im_[sz(k)] = v.imag();
            }
            fft_.inverse(re_, im_, ir_);
            float* out = filters_.data() + (sz(c) * 2 + sz(e)) * sz(design_.taps);
            for (int t = 0; t < design_.taps; ++t)
                out[t] = static_cast<float>(ir_[sz(((t - design_.delay) % N + N) % N)] * window_[sz(t)]);
        }
}

std::span<const float> BinauralFilterBuilder::filter(int acn, int ear) const {
    return {filters_.data() + (sz(acn) * 2 + sz(ear)) * sz(design_.taps), sz(design_.taps)};
}

double BinauralFilterBuilder::magnitudeError(double loHz, double hiHz) const {
    std::vector<Cx> ears(sz(directions_) * 2);
    double sum = 0;
    int count = 0;
    for (int k = 0; k < bins(); ++k) {
        if (binHz(k) < loHz || binHz(k) >= hiHz) continue;
        decodedEars(k, ears);
        double e = 0;
        for (int d = 0; d < directions_; ++d) {
            const double left = 20.0 * std::log10(std::abs(ears[sz(d) * 2]) + 1e-9) - head_.left(d)[sz(k)];
            const double right = 20.0 * std::log10(std::abs(ears[sz(d) * 2 + 1]) + 1e-9) - head_.right(d)[sz(k)];
            e += weight_[sz(d)] * (left * left + right * right);
        }
        sum += std::sqrt(e / 2.0);
        ++count;
    }
    return count ? sum / count : 0.0;
}

double BinauralFilterBuilder::diffuseError(double loHz, double hiHz) const {
    double worst = 0;
    for (int k = 0; k < bins(); ++k) {
        if (binHz(k) < loHz || binHz(k) >= hiHz) continue;
        const Cx* b = decode_.data() + sz(k) * sz(channels_) * 2;
        double decoded[2] = {0, 0}, target[2] = {0, 0};
        for (int c = 0; c < channels_; ++c)
            for (int e = 0; e < 2; ++e) decoded[e] += diffuse_[sz(c)] * std::norm(b[2 * c + e]);
        for (int d = 0; d < directions_; ++d) {
            target[0] += weight_[sz(d)] * std::pow(10.0, head_.left(d)[sz(k)] / 10.0);
            target[1] += weight_[sz(d)] * std::pow(10.0, head_.right(d)[sz(k)] / 10.0);
        }
        for (int e = 0; e < 2; ++e) worst = std::max(worst, std::abs(10.0 * std::log10(decoded[e] / target[e])));
    }
    return worst;
}

}  // namespace bambi
