// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/headmodel.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }

bool sized(const std::vector<double>& v, int order, int points, const char* what, std::string* why) {
    if (order < 0 || order > kMaxOrder) {
        if (why) *why = std::string(what) + ": order out of range";
        return false;
    }
    if (v.size() != sz(numChannels(order)) * sz(points)) {
        if (why)
            *why = std::string(what) + ": " + std::to_string(v.size()) + " values, expected " +
                   std::to_string(numChannels(order) * points);
        return false;
    }
    return true;
}

/// Y (directions x channels, `stride` wide) times rows (channels x points) into out (directions x points).
void evaluate(std::span<const double> basis, int stride, int channels, std::span<const double> rows, int points,
              std::span<double> out) {
    const int directions = static_cast<int>(basis.size()) / stride;
    for (int d = 0; d < directions; ++d) {
        double* o = out.data() + sz(d) * sz(points);
        std::fill(o, o + points, 0.0);
        for (int c = 0; c < channels; ++c) {
            const double y = basis[sz(d) * sz(stride) + sz(c)];
            const double* r = rows.data() + sz(c) * sz(points);
            for (int k = 0; k < points; ++k) o[k] += y * r[k];
        }
    }
}

/// A table's rows at the wanted axis points: linear between the model's points, flat beyond its ends.
std::vector<double> resampled(const HeadModel::Table& t, const HeadModel::Axis& axis, std::span<const double> erbs) {
    const int channels = numChannels(t.order), points = static_cast<int>(erbs.size());
    std::vector<double> out(sz(channels) * sz(points));
    for (int k = 0; k < points; ++k) {
        const double p = std::clamp((erbs[sz(k)] - axis.erb0) / axis.step, 0.0, static_cast<double>(axis.points - 1));
        const int i = std::min(static_cast<int>(p), axis.points - 2);
        const double f = p - i;
        for (int c = 0; c < channels; ++c) {
            const double* r = t.coef.data() + sz(c) * sz(axis.points) + sz(i);
            out[sz(c) * sz(points) + sz(k)] = r[0] + (r[1] - r[0]) * f;
        }
    }
    return out;
}

double dot(std::span<const double> y, const std::vector<double>& coef) {
    double v = 0;
    for (std::size_t p = 0; p < coef.size(); ++p) v += y[p] * coef[p];
    return v;
}
}  // namespace

bool HeadModel::consistent(std::string* why) const {
    auto fail = [why](const char* what) {
        if (why) *why = what;
        return false;
    };
    if (axis.points < 2 || !(axis.step > 0)) return fail("axis: at least two points, a positive step");
    if (!sized(smooth.coef, smooth.order, axis.points, "smooth", why)) return false;
    if (!sized(ild.coef, ild.order, axis.points, "ild", why)) return false;
    if (ildWithSize.order != ild.order) return fail("ildWithSize: not ild's order");
    if (!sized(ildWithSize.coef, ildWithSize.order, axis.points, "ildWithSize", why)) return false;
    if (!sized(itd.coef, itd.order, 1, "itd", why)) return false;
    if (notches.order < 0 || notches.order > kMaxOrder) return fail("notches: order out of range");
    if (!(notches.fadeIn < notches.fadeOut)) return fail("notches: fade must rise");
    if (!(notches.bandLo < notches.bandHi)) return fail("notches: band must be a range");
    if (!(notches.widthMin > 0) || !(notches.widthMin <= notches.widthMax))
        return fail("notches: widths must be a positive range");
    if (!(notches.depthFactor >= 0)) return fail("notches: depth factor must not be negative");
    for (const Notch& n : notches.list)
        for (const std::vector<double>* m : {&n.centre, &n.depth, &n.width, &n.presence})
            if (!sized(*m, notches.order, 1, "a notch's map", why)) return false;
    return true;
}

void Head::prepare(const HeadModel& model, std::span<const Vec3> directions, std::span<const double> erbs) {
    numDirections_ = static_cast<int>(directions.size());
    numNotches_ = static_cast<int>(model.notches.list.size());
    bandLo_ = model.notches.bandLo;
    bandHi_ = model.notches.bandHi;

    erb_.assign(erbs.begin(), erbs.end());
    if (erb_.empty()) {
        erb_.resize(sz(model.axis.points));
        for (int k = 0; k < model.axis.points; ++k) erb_[sz(k)] = model.axis.erb0 + k * model.axis.step;
    }
    numPoints_ = static_cast<int>(erb_.size());

    //  One basis per direction at the highest order any map needs; a lower order is its prefix (ACN).
    const int order = std::max({model.smooth.order, model.ild.order, model.itd.order, model.notches.order});
    const int stride = numChannels(order);
    std::vector<double> basis(sz(numDirections_) * sz(stride));
    for (int d = 0; d < numDirections_; ++d) {
        Vec3 v = directions[sz(d)];
        const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (n > 0) v = v * (1.0 / n);
        shSN3D(v, order, std::span<double>(basis.data() + sz(d) * sz(stride), sz(stride)));
    }

    const std::size_t cells = sz(numDirections_) * sz(numPoints_);
    smooth_.resize(cells);
    ildBase_.resize(cells);
    ildSlope_.resize(cells);
    left_.resize(cells);
    right_.resize(cells);
    evaluate(basis, stride, numChannels(model.smooth.order), resampled(model.smooth, model.axis, erb_), numPoints_,
             smooth_);
    evaluate(basis, stride, numChannels(model.ild.order), resampled(model.ild, model.axis, erb_), numPoints_, ildBase_);
    evaluate(basis, stride, numChannels(model.ildWithSize.order), resampled(model.ildWithSize, model.axis, erb_),
             numPoints_, ildSlope_);

    itdBase_.resize(sz(numDirections_));
    itd_.resize(sz(numDirections_));
    const std::size_t count = sz(numDirections_) * sz(numNotches_);
    centre_.resize(count);
    depth_.resize(count);
    width_.resize(count);
    shifted_.resize(sz(numNotches_));
    const HeadModel::Notches& N = model.notches;
    for (int d = 0; d < numDirections_; ++d) {
        const std::span<const double> y(basis.data() + sz(d) * sz(stride), sz(stride));
        itdBase_[sz(d)] = dot(y, model.itd.coef);
        for (int n = 0; n < numNotches_; ++n) {
            const HeadModel::Notch& notch = N.list[sz(n)];
            const std::size_t i = sz(d) * sz(numNotches_) + sz(n);
            //  Presence fades the notch in with a smoothstep; the depth is floored at -60 dB.
            const double t = std::clamp((dot(y, notch.presence) - N.fadeIn) / (N.fadeOut - N.fadeIn), 0.0, 1.0);
            const double fade = t * t * (3 - 2 * t);
            centre_[i] = dot(y, notch.centre);
            depth_[i] = std::max(-60.0, std::min(0.0, dot(y, notch.depth)) * N.depthFactor * fade);
            width_[i] = 1.0 / std::clamp(dot(y, notch.width), N.widthMin, N.widthMax);
        }
    }
}

void Head::synthesise(double earShiftErb, double headSize) {
    const double grow = headSize - 1.0;
    for (int d = 0; d < numDirections_; ++d) {
        itd_[sz(d)] = itdBase_[sz(d)] * headSize;
        const std::size_t at = sz(d) * sz(numNotches_);
        for (int n = 0; n < numNotches_; ++n)
            shifted_[sz(n)] = std::clamp(centre_[at + sz(n)] + earShiftErb, bandLo_, bandHi_);

        const std::size_t cell = sz(d) * sz(numPoints_);
        const double* smooth = smooth_.data() + cell;
        const double* base = ildBase_.data() + cell;
        const double* slope = ildSlope_.data() + cell;
        double* left = left_.data() + cell;
        double* right = right_.data() + cell;
        for (int k = 0; k < numPoints_; ++k) {
            //  Each notch is a peaking filter's dip in dB: depth / (1 + u^2), u the distance from the centre in widths.
            double cll = smooth[k];
            for (int n = 0; n < numNotches_; ++n) {
                const double a = depth_[at + sz(n)];
                if (a == 0.0) continue;
                const double u = (erb_[sz(k)] - shifted_[sz(n)]) * width_[at + sz(n)];
                cll += a / (1.0 + u * u);
            }
            //  The two ears share the summed level cll and differ by ild: left = cll + 10 log10(r / (1 + r)),
            //  right = cll - 10 log10(1 + r) with r = 10^(ild / 10).
            const double ild = base[k] + grow * slope[k];
            const double both = 4.342944819032518 * std::log1p(std::exp(ild * 0.23025850929940458));  // 10 log10(1 + r)
            left[k] = cll + ild - both;
            right[k] = cll - both;
        }
    }
}

}  // namespace bambi
