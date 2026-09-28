// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/ping.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/echo/warp.hpp"

namespace bambi {

namespace {

/*  An orthonormal frame with `axis` as z. x and y are arbitrary; the spin is applied and undone in
 *  the same frame, so the choice cancels. */
void frameFor(Vec3 axis, Vec3& x, Vec3& y, Vec3& z) {
    z = unit(axis);
    const Vec3 away = std::abs(z.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    x = unit(cross(away, z));
    y = cross(z, x);
}

}  // namespace

PingAt loopAt(const TapSettings& tap, double passes) {
    //  A skew is tanh of a rapidity and passes add rapidities, so a fraction of a pass has a skew
    //  of its own and the trace has no corner at whole passes.
    const double p = std::max(0.0, passes);
    const double rapidity = std::atanh(clampd(tap.skew, -kMaxSkew, kMaxSkew));
    return {passes * tap.spinRad, std::tanh(p * rapidity), tap.blurRad * passes};
}

Vec3 imageAt(const TapSettings& tap, Vec3 from, const PingAt& at) {
    Vec3 ex, ey, ez;
    frameFor(tap.axis, ex, ey, ez);
    const double px = dot(from, ex), py = dot(from, ey), pz = dot(from, ez);

    //  the spin, about the axis
    const double c = std::cos(at.spinRad), s = std::sin(at.spinRad);
    double sx = px * c - py * s, sy = px * s + py * c;

    //  and the slide along it
    const double z2 = slideZ(clampd(pz, -1.0, 1.0), at.skew);
    const double r1 = std::hypot(sx, sy);
    const double r2 = std::sqrt(std::max(0.0, 1.0 - z2 * z2));
    const double scale = r1 > 1e-12 ? r2 / r1 : 0.0;
    return unit(ex * (sx * scale) + ey * (sy * scale) + ez * z2);
}

void pingTrace(const TapSettings& tap, Vec3 from, int passes, std::vector<Vec3>& into, std::vector<float>* at) {
    into.clear();
    if (at != nullptr) at->clear();
    if (passes <= 0) return;

    //  Sampled in degrees of turn, so a circle stays round.
    const double turnDeg = std::abs(tap.spinRad) * kRad2Deg * passes;
    const int steps = std::clamp(static_cast<int>(std::lround(turnDeg / kPingStepDeg)), kPingMinSteps, kPingMaxSteps);
    const Vec3 seed = unit(from);
    into.reserve(static_cast<std::size_t>(steps) + 1);
    for (int k = 0; k <= steps; ++k) {
        const double u = static_cast<double>(k) / steps;
        into.push_back(imageAt(tap, seed, loopAt(tap, u * passes)));
        if (at != nullptr) at->push_back(static_cast<float>(u));
    }
}

void flowSeeds(const TapSettings& tap, std::array<Vec3, kFlowSpokes>& into) {
    Vec3 ex, ey, ez;
    frameFor(tap.axis, ex, ey, ez);
    for (int j = 0; j < kFlowSpokes; ++j) {
        const double a = 2.0 * kPi * static_cast<double>(j) / kFlowSpokes;
        into[static_cast<std::size_t>(j)] = unit(ex * std::cos(a) + ey * std::sin(a));
    }
}

std::vector<PingBead> pingSpiral(const TapSettings& tap, Vec3 from, int passes, double level, double feedback) {
    std::vector<PingBead> out;
    if (passes <= 0) return out;
    out.reserve(static_cast<std::size_t>(passes));

    Vec3 at = unit(from);
    double gain = level;
    for (int j = 1; j <= passes; ++j) {
        at = pingStep(tap, at);
        out.push_back({at, gain, tap.blurRad * std::sqrt(static_cast<double>(j)), j});
        gain *= feedback;
    }
    return out;
}

}  // namespace bambi
