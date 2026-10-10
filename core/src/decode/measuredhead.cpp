// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/measuredhead.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/dsp/fft.hpp"
#include "bambi/math/lstsq.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }
constexpr int kSpectrumSize = 4096;  // the responses zero-padded: 11.7 Hz bins at 48 kHz
constexpr int kItdSize = 4096, kItdUpsample = 8, kItdSearchSamples = 80;
constexpr double kItdCutoffHz = 1500.0;

double erbToHz(double erb) { return (std::pow(10.0, erb / 21.4) - 1.0) / 0.00437; }

/// Both ears' magnitudes, dB, on the axis's points: the spectrum's bins interpolated linearly in dB.
struct Magnitudes {
    RealFft fft;
    std::vector<double> in, re, im, levelL, levelR;  // levels: points each
    std::vector<double> hz;                          // the axis's frequencies

    bool prepare(const HeadModel::Axis& axis, double sampleRate) {
        if (!fft.prepare(sz(kSpectrumSize))) return false;
        in.assign(sz(kSpectrumSize), 0.0);
        re.resize(sz(kSpectrumSize / 2 + 1));
        im.resize(sz(kSpectrumSize / 2 + 1));
        hz.resize(sz(axis.points));
        for (int k = 0; k < axis.points; ++k) hz[sz(k)] = erbToHz(axis.erb0 + k * axis.step);
        levelL.resize(sz(axis.points));
        levelR.resize(sz(axis.points));
        rate_ = sampleRate;
        return true;
    }
    void ear(std::span<const float> ir, std::vector<double>& level) {
        std::fill(in.begin(), in.end(), 0.0);
        const std::size_t n = std::min(ir.size(), in.size());
        for (std::size_t i = 0; i < n; ++i) in[i] = ir[i];
        fft.forward(in, re, im);
        const double binHz = rate_ / kSpectrumSize;
        const int bins = kSpectrumSize / 2 + 1;
        for (std::size_t k = 0; k < hz.size(); ++k) {
            const double pos = std::clamp(hz[k] / binHz, 0.0, static_cast<double>(bins - 1));
            const int b = std::min(static_cast<int>(pos), bins - 2);
            const double t = pos - b;
            const auto db = [&](int i) {
                return 10.0 * std::log10(std::max(re[sz(i)] * re[sz(i)] + im[sz(i)] * im[sz(i)], 1e-20));
            };
            level[k] = (1.0 - t) * db(b) + t * db(b + 1);
        }
    }
    void both(std::span<const float> left, std::span<const float> right) {
        ear(left, levelL);
        ear(right, levelR);
    }

private:
    double rate_{48000.0};
};

/// The summed level and the difference from both ears' levels, the difference held to the limit.
void cllIld(const std::vector<double>& l, const std::vector<double>& r, double ildLimitDb, double* cll, double* ild) {
    for (std::size_t k = 0; k < l.size(); ++k) {
        cll[k] = 10.0 * std::log10(std::pow(10.0, l[k] / 10.0) + std::pow(10.0, r[k] / 10.0));
        ild[k] = std::clamp(l[k] - r[k], -ildLimitDb, ildLimitDb);
    }
}

bool shapeOk(const MeasuredHead& m, std::string* error) {
    const auto fail = [error](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (m.directions.empty()) return fail("no directions");
    if (m.taps <= 0) return fail("no taps");
    if (!(m.sampleRate > 0)) return fail("no sample rate");
    const std::size_t cells = m.directions.size() * sz(m.taps);
    if (m.left.size() != cells || m.right.size() != cells) return fail("the responses do not fit directions x taps");
    return true;
}

/// Fits one table (directions x points, or x 1) over the basis with the roughness penalty: coef is channels x
/// points. `ginv` is the penalised Gram's inverse for `channels`.
void fitTable(std::span<const double> basis, int stride, int channels, std::span<const double> weights,
              const std::vector<double>& ginv, std::span<const double> values, int points,
              std::vector<double>& coef) {
    const int directions = static_cast<int>(weights.size());
    //  Y^T W V first, channels x points, then the inverse Gram on it: never a channels x directions matrix.
    std::vector<double> ytwv(sz(channels) * sz(points), 0.0);
    for (int d = 0; d < directions; ++d) {
        const double* y = basis.data() + sz(d) * sz(stride);
        const double* v = values.data() + sz(d) * sz(points);
        const double w = weights[sz(d)];
        for (int c = 0; c < channels; ++c) {
            const double wy = w * y[c];
            double* row = ytwv.data() + sz(c) * sz(points);
            for (int k = 0; k < points; ++k) row[k] += wy * v[k];
        }
    }
    coef.assign(sz(channels) * sz(points), 0.0);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < channels; ++i) {
            const double g = ginv[sz(c) * sz(channels) + sz(i)];
            if (g == 0.0) continue;
            const double* row = ytwv.data() + sz(i) * sz(points);
            double* out = coef.data() + sz(c) * sz(points);
            for (int k = 0; k < points; ++k) out[k] += g * row[k];
        }
}

/// (Y^T W Y + penalty)^-1 for the first `channels` of the basis.
void penalisedInverse(std::span<const double> basis, int stride, int channels, std::span<const double> weights,
                      double roughness, std::vector<double>& ginv) {
    //  the basis's prefix, contiguous, as gramMatrix wants it
    const int directions = static_cast<int>(weights.size());
    std::vector<double> prefix(sz(directions) * sz(channels));
    for (int d = 0; d < directions; ++d)
        std::copy_n(basis.data() + sz(d) * sz(stride), channels, prefix.data() + sz(d) * sz(channels));
    gramMatrix(prefix, weights, channels, ginv);
    double trace = 0.0;
    for (int c = 0; c < channels; ++c) trace += ginv[sz(c) * sz(channels) + sz(c)];
    const double scale = roughness * trace / channels;
    for (int c = 0; c < channels; ++c) {
        const double n = acnOrder(c);
        ginv[sz(c) * sz(channels) + sz(c)] += scale * n * (n + 1.0);
    }
    invertSpd(ginv, channels);
}
}  // namespace

double itdMicroseconds(std::span<const float> left, std::span<const float> right, double sampleRate) {
    RealFft fft, up;
    if (!fft.prepare(sz(kItdSize)) || !up.prepare(sz(kItdSize * kItdUpsample))) return 0.0;
    std::vector<double> l(sz(kItdSize), 0.0), r(sz(kItdSize), 0.0);
    for (std::size_t i = 0; i < std::min(left.size(), l.size()); ++i) l[i] = left[i];
    for (std::size_t i = 0; i < std::min(right.size(), r.size()); ++i) r[i] = right[i];
    const int bins = kItdSize / 2 + 1;
    std::vector<double> lr(sz(bins)), li(sz(bins)), rr(sz(bins)), ri(sz(bins));
    fft.forward(l, lr, li);
    fft.forward(r, rr, ri);
    //  the cross spectrum L R*, weighted by |B|^4 of a fourth-order Butterworth at the cutoff, on the long grid
    const int upBins = kItdSize * kItdUpsample / 2 + 1;
    std::vector<double> cr(sz(upBins), 0.0), ci(sz(upBins), 0.0);
    for (int k = 0; k < bins; ++k) {
        const double f = k * sampleRate / kItdSize;
        const double w = 1.0 / std::pow(1.0 + std::pow(f / kItdCutoffHz, 8.0), 2.0);
        cr[sz(k)] = w * (lr[sz(k)] * rr[sz(k)] + li[sz(k)] * ri[sz(k)]);
        ci[sz(k)] = w * (li[sz(k)] * rr[sz(k)] - lr[sz(k)] * ri[sz(k)]);
    }
    std::vector<double> cc(sz(kItdSize * kItdUpsample));
    up.inverse(cr, ci, cc);
    //  the peak within the search window, lags wrapping round the end; then the parabola through its neighbours
    const int span = kItdSearchSamples * kItdUpsample, n = static_cast<int>(cc.size());
    int best = 0;
    double bestValue = -1e300;
    for (int lag = -span; lag <= span; ++lag) {
        const double v = cc[sz((lag + n) % n)];
        if (v > bestValue) {
            bestValue = v;
            best = lag;
        }
    }
    const double a = cc[sz((best - 1 + n) % n)], b = bestValue, c = cc[sz((best + 1 + n) % n)];
    const double denom = a - 2.0 * b + c;
    const double refine = std::abs(denom) > 1e-300 ? std::clamp(0.5 * (a - c) / denom, -1.0, 1.0) : 0.0;
    return (best + refine) / kItdUpsample / sampleRate * 1e6;
}

bool fitHeadModel(const MeasuredHead& m, const HeadFit& fit, HeadModel& out, std::string* error) {
    const auto fail = [error](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (!shapeOk(m, error)) return false;
    if (fit.order < 0 || fit.order > kMaxOrder || fit.itdOrder < 0 || fit.itdOrder > kMaxOrder)
        return fail("an order out of range");
    if (fit.axis.points < 2 || !(fit.axis.step > 0)) return fail("the axis needs two points and a step");
    const int directions = static_cast<int>(m.directions.size());
    const int channels = numChannels(fit.order), itdChannels = numChannels(fit.itdOrder);
    if (directions < std::max(channels, itdChannels) && fit.roughness <= 0.0)
        return fail("fewer directions than channels, and no roughness penalty to hold the fit");

    Magnitudes mag;
    if (!mag.prepare(fit.axis, m.sampleRate)) return fail("the spectrum's FFT could not be prepared");
    const int points = fit.axis.points;
    std::vector<double> cll(sz(directions) * sz(points)), ild(sz(directions) * sz(points)), itd(sz(directions));
    for (int d = 0; d < directions; ++d) {
        const std::span<const float> l(m.left.data() + sz(d) * sz(m.taps), sz(m.taps));
        const std::span<const float> r(m.right.data() + sz(d) * sz(m.taps), sz(m.taps));
        mag.both(l, r);
        cllIld(mag.levelL, mag.levelR, fit.ildLimitDb, cll.data() + sz(d) * sz(points), ild.data() + sz(d) * sz(points));
        itd[sz(d)] = itdMicroseconds(l, r, m.sampleRate);
    }

    //  every direction weighs the same; the penalty holds the fit where the grid has no points
    const int stride = numChannels(std::max(fit.order, fit.itdOrder));
    std::vector<double> basis(sz(directions) * sz(stride));
    for (int d = 0; d < directions; ++d) {
        Vec3 v = unit(m.directions[sz(d)]);
        shSN3D(v, std::max(fit.order, fit.itdOrder), std::span<double>(basis.data() + sz(d) * sz(stride), sz(stride)));
    }
    const std::vector<double> weights(sz(directions), 1.0 / directions);
    std::vector<double> ginv;
    penalisedInverse(basis, stride, channels, weights, fit.roughness, ginv);
    out.axis = fit.axis;
    out.smooth.order = fit.order;
    fitTable(basis, stride, channels, weights, ginv, cll, points, out.smooth.coef);
    out.ild.order = fit.order;
    fitTable(basis, stride, channels, weights, ginv, ild, points, out.ild.coef);
    out.ildWithSize.order = fit.order;
    out.ildWithSize.coef.assign(sz(channels) * sz(points), 0.0);
    penalisedInverse(basis, stride, itdChannels, weights, fit.roughness, ginv);
    out.itd.order = fit.itdOrder;
    fitTable(basis, stride, itdChannels, weights, ginv, itd, 1, out.itd.coef);

    //  no notch layer: the head's notches are in its tables; the ranges only have to make sense
    HeadModel::Notches& N = out.notches;
    N = HeadModel::Notches{};
    N.order = 0;
    N.depthFactor = 1.0;
    N.fadeIn = 0.0;
    N.fadeOut = 1.0;
    N.bandLo = fit.axis.erb0;
    N.bandHi = fit.axis.erb0 + (fit.axis.points - 1) * fit.axis.step;
    N.widthMin = 0.5;
    N.widthMax = 2.0;
    N.list.clear();
    out.learnedFrom = m.name;
    return out.consistent(error);
}

HeadFitError headFitError(const MeasuredHead& m, const HeadModel& model, double loHz, double hiHz,
                          const HeadModel::Axis* axis) {
    HeadFitError e;
    if (!shapeOk(m, nullptr)) return e;
    const HeadModel::Axis on = axis != nullptr ? *axis : model.axis;
    std::vector<double> erbs(sz(on.points));
    for (int k = 0; k < on.points; ++k) erbs[sz(k)] = on.erb0 + k * on.step;
    Head head;
    head.prepare(model, m.directions, erbs);
    head.synthesise(0.0, 1.0);
    Magnitudes mag;
    if (!mag.prepare(on, m.sampleRate)) return e;
    const int points = head.points();
    std::vector<double> cll(sz(points)), ild(sz(points));
    double sumCll = 0, sumIld = 0, sumItd = 0;
    std::size_t cells = 0;
    const int directions = static_cast<int>(m.directions.size());
    for (int d = 0; d < directions; ++d) {
        const std::span<const float> l(m.left.data() + sz(d) * sz(m.taps), sz(m.taps));
        const std::span<const float> r(m.right.data() + sz(d) * sz(m.taps), sz(m.taps));
        mag.both(l, r);
        cllIld(mag.levelL, mag.levelR, 1e9, cll.data(), ild.data());
        const std::span<const double> left = head.left(d), right = head.right(d);
        for (int k = 0; k < points; ++k) {
            const double hz = mag.hz[sz(k)];
            if (hz < loHz || hz >= hiHz) continue;
            const double haveCll = 10.0 * std::log10(std::pow(10.0, left[sz(k)] / 10.0) + std::pow(10.0, right[sz(k)] / 10.0));
            const double dc = haveCll - cll[sz(k)], di = (left[sz(k)] - right[sz(k)]) - ild[sz(k)];
            sumCll += dc * dc;
            sumIld += di * di;
            ++cells;
        }
        const double dt = head.itd(d) - itdMicroseconds(l, r, m.sampleRate);
        sumItd += dt * dt;
    }
    if (cells > 0) {
        e.cllDb = std::sqrt(sumCll / static_cast<double>(cells));
        e.ildDb = std::sqrt(sumIld / static_cast<double>(cells));
    }
    e.itdUs = std::sqrt(sumItd / directions);
    return e;
}

}  // namespace bambi
