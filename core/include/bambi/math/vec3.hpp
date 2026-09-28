// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>

namespace bambi {

/*  Coordinate convention:
 *
 *      x = front      y = left      z = up        (right-handed)
 *      azimuth   counter-clockwise from front
 *      elevation positive up
 *
 *  This is AmbiX. Everything downstream assumes it, and test_vec3.cpp fails loudly if
 *  anyone "corrects" it. Angles are in radians everywhere inside the core; degrees exist
 *  only at the parameter and UI boundary.
 */

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeg2Rad = kPi / 180.0;
inline constexpr double kRad2Deg = 180.0 / kPi;

struct Vec3 {
    double x{}, y{}, z{};

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr Vec3 operator*(double s, Vec3 a) { return a * s; }

constexpr double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

inline double length(Vec3 a) { return std::sqrt(dot(a, a)); }

inline Vec3 unit(Vec3 a) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec3{1.0, 0.0, 0.0};
}

constexpr double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Unit vector from azimuth and elevation, both in radians.
inline Vec3 fromAzEl(double azRad, double elRad) {
    const double ce = std::cos(elRad);
    return {ce * std::cos(azRad), ce * std::sin(azRad), std::sin(elRad)};
}

/// Azimuth in radians, in (-pi, pi].
inline double azimuth(Vec3 p) { return std::atan2(p.y, p.x); }

/// Elevation in radians, in [-pi/2, pi/2].
inline double elevation(Vec3 p) { return std::asin(clampd(p.z, -1.0, 1.0)); }

/*  Great-circle angle between two unit vectors, in radians.
 *
 *  Precision floor: this cannot resolve angles below about 1e-8 rad. acos is ill-
 *  conditioned near 1 — its derivative is 1/sqrt(1-x^2) — so a dot product accurate to
 *  1e-16 yields an angle accurate only to sqrt(2 * 1e-16) ~= 1.5e-8. Anything comparing
 *  near-identical directions must allow for that; it is a property of the formula, not of
 *  the inputs. Irrelevant perceptually (1e-8 rad is 6e-7 degrees) but it does set the
 *  tolerance any test can ask for.
 */
inline double arc(Vec3 a, Vec3 b) { return std::acos(clampd(dot(a, b), -1.0, 1.0)); }

/*  Are two directions the same to within `tol` per component?
 *
 *  Use this, not `arc(a,b) < tol`, whenever the expected answer is exactly zero. arc() has
 *  a hard floor around 1.5e-8 rad (see the note above) and arc(p, p) for a p that is unit
 *  only to rounding already returns about that -- so an identity assertion written with
 *  arc() cannot be made tighter than the floor no matter how correct the code is. */
inline bool sameDir(Vec3 a, Vec3 b, double tol = 1e-12) {
    return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol && std::abs(a.z - b.z) < tol;
}

}  // namespace bambi
