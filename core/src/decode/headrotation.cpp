// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/headrotation.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/dsp/field.hpp"
#include "bambi/dsp/ramp.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {
std::size_t sz(int n) { return static_cast<std::size_t>(n); }

/// How far `a` is from `b`: the angle of the rotation that carries b onto a, and its axis (unit, or zero when the
/// angle is).
double between(const Mat3& a, const Mat3& b, Vec3& axis) {
    const Mat3 r = multiply(a, transposed(b));
    const double trace = r[0] + r[4] + r[8];
    const double angle = std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0));
    axis = {r[7] - r[5], r[2] - r[6], r[3] - r[1]};
    const double len = std::sqrt(dot(axis, axis));
    axis = len > 1e-12 ? axis * (1.0 / len) : Vec3{0, 0, 0};
    return angle;
}
}  // namespace

bool HeadRotation::prepare(int order, double sampleRate) {
    if (order < 0 || order > kFixedOrder || !(sampleRate > 0)) return false;
    order_ = order;
    channels_ = numChannels(order);
    alpha_ = 1.0 - std::exp(-kRotationHop / (sampleRate * kRotationGlideSeconds));
    blocks_.assign(sz(rotationSize(order)), 0.0);
    forward_.assign(sz(rotationSize(order)), 0.0f);
    previous_.assign(sz(rotationSize(order)), 0.0f);
    inverseScratch_.assign(sz(rotationSize(order)), 0.0f);
    a_.assign(sz(kRotationHop) * sz(channels_), 0.0f);
    b_.assign(sz(kRotationHop) * sz(channels_), 0.0f);
    goal_ = current_ = kIdentity3;
    turnTo(current_);
    snap_ = true;
    settled_ = true;
    return true;
}

void HeadRotation::turnTo(const Mat3& head) noexcept {
    // The field turns by the inverse of the head: what the head turns toward comes to the front.
    shRotation(transposed(head), order_, blocks_);
    fieldBlocks(blocks_, order_, forward_, inverseScratch_);
}

void HeadRotation::process(const float* in, float* out, int frames) noexcept {
    if (blocks_.empty()) return;
    for (int done = 0; done < frames;) {
        // Hops are counted along the stream, not per call, so a glide step lands on the same frame however the
        // frames are cut.
        const int n = std::min(kRotationHop - pos_, frames - done);
        const float* x = in + sz(done) * sz(channels_);
        float* y = out + sz(done) * sz(channels_);

        if (pos_ == 0) {
            if (const Mat3* fresh = target_.latest()) {
                goal_ = *fresh;
                settled_ = false;
            }
            moving_ = false;
            if (!settled_) {
                Vec3 axis;
                const double angle = between(goal_, current_, axis);
                if (snap_ || angle < 1e-5 || dot(axis, axis) == 0.0) {
                    current_ = goal_;
                    settled_ = true;
                    snap_ = false;
                } else {
                    current_ = multiply(rotationAbout(axis, angle * alpha_), current_);
                }
                std::swap(previous_, forward_);
                turnTo(current_);
                moving_ = true;
            }
        }
        if (moving_) {
            // Both turns over the hop, mixed with a ramp, so a step in orientation never steps in sound.
            rotateField(previous_, order_, x, a_.data(), n);
            rotateField(forward_, order_, x, b_.data(), n);
            for (int f = 0; f < n; ++f) {
                const float t = static_cast<float>(rampWeight(pos_ + f + 1, kRotationHop));
                for (int c = 0; c < channels_; ++c) {
                    const std::size_t i = sz(f) * sz(channels_) + sz(c);
                    y[i] = a_[i] * (1.0f - t) + b_[i] * t;
                }
            }
        } else {
            rotateField(forward_, order_, x, y, n);
        }
        pos_ = (pos_ + n) % kRotationHop;
        done += n;
    }
}

}  // namespace bambi
