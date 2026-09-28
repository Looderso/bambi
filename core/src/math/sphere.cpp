// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/math/sphere.hpp"

#include <cmath>

namespace bambi {

Vec3 expMap(Vec3 p, Vec3 v) {
    const double t = length(v);
    if (t < 1e-12) return p;
    return unit(p * std::cos(t) + v * (std::sin(t) / t));
}

Vec3 logMap(Vec3 p, Vec3 q) {
    const double th = arc(p, q);
    if (th < 1e-12) return {0, 0, 0};
    const Vec3 d = q - p * std::cos(th);
    const double l = length(d);
    return l < 1e-12 ? Vec3{0, 0, 0} : d * (th / l);
}

Vec3 tangentAt(Vec3 p, Vec3 d) {
    const Vec3 t = d - p * dot(p, d);
    if (length(t) > 1e-12) return t;
    // p is parallel to d: any tangent will do, so pick one that cannot also be degenerate.
    const Vec3 alt = std::abs(p.z) > 0.9 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    return alt - p * dot(p, alt);
}

Vec3 rotateAxis(Vec3 v, Vec3 k, double theta) {
    const double c = std::cos(theta), s = std::sin(theta);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

Vec3 rotateAToB(Vec3 v, Vec3 a, Vec3 b) {
    const Vec3 ax = cross(a, b);
    const double s = length(ax);
    if (s < 1e-12) return dot(a, b) > 0 ? v : v * -1.0;  // identical or antipodal
    return rotateAxis(v, ax * (1.0 / s), std::atan2(s, dot(a, b)));
}

Vec3 slerp(Vec3 a, Vec3 b, double t) {
    const double w = arc(a, b);
    if (w < 1e-9) return a;
    const double sw = std::sin(w);
    return unit(a * (std::sin((1.0 - t) * w) / sw) + b * (std::sin(t * w) / sw));
}

}  // namespace bambi
