// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "bambi/math/vec3.hpp"

namespace bambi {

/*  Motion on the unit sphere. Everything here takes and returns unit vectors. */

/// Walk from p along tangent v, where |v| is the distance in radians.
Vec3 expMap(Vec3 p, Vec3 v);

/// The tangent at p that walks to q. |result| is the great-circle distance.
Vec3 logMap(Vec3 p, Vec3 q);

/// Project d into the tangent plane at p. Falls back to an arbitrary tangent if degenerate.
Vec3 tangentAt(Vec3 p, Vec3 d);

/// Rodrigues: rotate v about unit axis k by theta radians.
Vec3 rotateAxis(Vec3 v, Vec3 k, double theta);

/// Apply to v the rotation that carries unit vector a onto unit vector b.
Vec3 rotateAToB(Vec3 v, Vec3 a, Vec3 b);

/// Spherical linear interpolation between unit vectors.
Vec3 slerp(Vec3 a, Vec3 b, double t);

}  // namespace bambi
