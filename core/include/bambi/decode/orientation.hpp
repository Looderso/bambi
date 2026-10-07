// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>
#include <string_view>

#include "bambi/math/shrotation.hpp"

/*  A tracker's numbers made into the head's orientation in the world: the receiver's conversion.
 *  Orientation arrives as a quaternion or as yaw, pitch and roll, in one of a few conventions; either
 *  becomes the rotation of the device in a gravity-aligned world (z up), is mapped from the device's
 *  axes to the head's (x front, y left, z up) for how the device is worn, and has the yaw at recentre
 *  taken off -- pitch and roll stay against gravity. Degrees at this boundary; the matrices inside.
 *
 *  The head's own angles: yaw left-positive, pitch up-positive, roll right-ear-down positive, as
 *  Rz(yaw) Ry(-pitch) Rx(roll). ADM-OSC sends exactly that; IEM and SPARTA send pitch down-positive
 *  (a plain rotation about +y); quaternions carry no such choice but come w-first or w-last.
 */
namespace bambi {

enum class OrientationForm {
    QuaternionWxyz,       ///< w, x, y, z: SPARTA, IEM, the AirPods helper
    QuaternionXyzw,       ///< x, y, z, w: Android's /rotationvector
    YprDegrees,           ///< yaw, pitch, roll in degrees, pitch up-positive: ADM-OSC
    YprDegreesPitchDown,  ///< the same with pitch down-positive: IEM, SPARTA
    YprRadians,           ///< yaw, pitch, roll in radians, pitch up-positive
};

/// How the device sits on the head: which of its axes are the head's front, left and up.
enum class Mounting {
    AsSent,        ///< the device reports the head's axes already: a head tracker, the AirPods
    PhoneTopEdge,  ///< a phone on the headphones' band, screen up, top edge forward
    PhoneBack,     ///< a phone held upright in front, its screen facing the listener
};

/// One way of reading what a sender sends, as learn mode or a preset settles it.
struct OrientationRead {
    OrientationForm form{OrientationForm::QuaternionWxyz};
    Mounting mounting{Mounting::AsSent};
    std::array<bool, 3> flip{false, false, false};  ///< a quaternion's x, y, z, or the yaw, pitch and roll, negated
};

/// The head's orientation from its angles, degrees.
Mat3 headFromYpr(double yawDeg, double pitchDeg, double rollDeg);
/// The head's angles back from its orientation, degrees: yaw in (-180, 180], pitch in [-90, 90], roll in (-180, 180].
void yprOf(const Mat3& head, double& yawDeg, double& pitchDeg, double& rollDeg);
/// The yaw alone, degrees: where the head's front points, around the vertical.
double yawDegOf(const Mat3& head);

/// `values` as `read` says, into the head's orientation in the world. False, and `out` untouched, when there are
/// too few values or one is not a number; a quaternion is normalised first.
bool headFromValues(const OrientationRead& read, std::span<const float> values, Mat3& out);

/// The head with `yawDeg` taken off: pitch and roll stay against gravity, so recentring while looking down and then
/// turning keeps the head looking down, with no false roll.
Mat3 recentred(const Mat3& head, double yawDeg);

/// The form the well-known addresses carry -- ADM-OSC's /adm/lis/ypr, SPARTA's /ypr and /quaternion, IEM's
/// /SceneRotator/ypr and /SceneRotator/quaternions, Android's /rotationvector -- or false for any other address,
/// which learn mode reads.
bool knownForm(std::string_view address, OrientationForm& out);

}  // namespace bambi
