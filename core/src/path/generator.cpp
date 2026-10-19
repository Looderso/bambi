// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/path/generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/math/sphere.hpp"

namespace bambi {
namespace {

double p(std::span<const double> a, std::size_t i, double def = 0.0) { return i < a.size() ? a[i] : def; }

/// Corner `k` of `sides` around the front, `radius` from it (radians): the first is above the front at
/// rotation 0, and a rotation turns the corners toward the left, as an azimuth does.
Vec3 corner(int k, int sides, double radius, double rotation) {
    const double a = rotation + 2.0 * kPi * k / sides;
    return expMap({1, 0, 0}, Vec3{0, std::sin(a), std::cos(a)} * radius);
}

}  // namespace

bool generatorIsClosed(GeneratorType g) { return g != GeneratorType::Spiral; }

int generatorCorners(GeneratorType g, std::span<const double> gp) {
    return g == GeneratorType::Polygon ? std::clamp(static_cast<int>(std::lround(p(gp, 0, 3.0))), 1, 8) : 1;
}

Vec3 generatorAt(GeneratorType g, double u, std::span<const double> gp) {
    switch (g) {
        case GeneratorType::Orbit: {  // tilt, spin, aperture
            const double a = u * 2.0 * kPi;
            const double r = p(gp, 2, 90.0) * kDeg2Rad;
            const Vec3 base{std::cos(r), std::sin(r) * std::cos(a), std::sin(r) * std::sin(a)};
            const Vec3 tilted = rotateAxis(base, {0, 1, 0}, p(gp, 0) * kDeg2Rad);
            return unit(rotateAxis(tilted, {0, 0, 1}, p(gp, 1) * kDeg2Rad));
        }
        case GeneratorType::Lissajous: {  // az_amount, el_amount, az_ratio, el_ratio, phase
            const double t = u * 2.0 * kPi;
            const double az = p(gp, 0, 80.0) * std::sin(p(gp, 2, 1.0) * t + p(gp, 4) * kDeg2Rad);
            const double el = p(gp, 1, 45.0) * std::sin(p(gp, 3, 2.0) * t);
            return fromAzEl(az * kDeg2Rad, el * kDeg2Rad);
        }
        case GeneratorType::Wave: {  // turns, el_amount, wobbles, el_offset
            const double t = u * 2.0 * kPi;
            const double az = u * 360.0 * p(gp, 0, 1.0);
            const double el = clampd(p(gp, 3) + p(gp, 1, 35.0) * std::sin(p(gp, 2, 3.0) * t), -89.0, 89.0);
            return fromAzEl(az * kDeg2Rad, el * kDeg2Rad);
        }
        case GeneratorType::Polygon: {  // sides, radius, rotation
            //  Every edge is as long as every other, so equal shares of u are equal lengths: the
            //  edges are great-circle arcs, and the corners stay sharp.
            const int sides = generatorCorners(g, gp);
            if (sides == 1) return {1, 0, 0};
            const double radius = p(gp, 1, 45.0) * kDeg2Rad, rotation = p(gp, 2) * kDeg2Rad;
            const double along = (u - std::floor(u)) * sides;
            const int k = std::min(static_cast<int>(along), sides - 1);
            return slerp(corner(k, sides, radius, rotation), corner((k + 1) % sides, sides, radius, rotation),
                         along - k);
        }
        case GeneratorType::Spiral: {  // turns, from_el, to_el, start_az
            const double az = p(gp, 3) + u * 360.0 * p(gp, 0, 2.0);
            const double el = p(gp, 1, 80.0) + (p(gp, 2, -80.0) - p(gp, 1, 80.0)) * u;
            return fromAzEl(az * kDeg2Rad, clampd(el, -89.0, 89.0) * kDeg2Rad);
        }
    }
    return {1, 0, 0};
}

void generatorDefaults(GeneratorType g, std::span<double> gp) {
    for (auto& v : gp) v = 0.0;
    const auto params = generatorParams(g);
    for (std::size_t i = 0; i < params.size() && i < gp.size(); ++i) gp[i] = params[i].def;
}

}  // namespace bambi
