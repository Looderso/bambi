// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/orientation.hpp"

#include <algorithm>
#include <cmath>

namespace bambi {

namespace {
/// A unit quaternion w, x, y, z as a rotation matrix, row-major.
Mat3 fromQuaternion(double w, double x, double y, double z) {
    return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
            2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
}

}  // namespace

/// Rows: the head's front, left and up, in the device's coordinates (a phone's: x right, y top edge, z out of the
/// screen). The device's rotation times this transposed is the head's.
Mat3 axesOfMounting(Mounting mounting, const Mat3& custom) {
    switch (mounting) {
        case Mounting::PhoneTopEdge:
        case Mounting::AirPods: return {0, 1, 0, -1, 0, 0, 0, 0, 1};  // the same frame: a top edge forward, z up
        case Mounting::PhoneBack: return {0, 0, -1, -1, 0, 0, 0, 1, 0};
        case Mounting::Axes: return custom;
        case Mounting::AsSent: break;
    }
    return kIdentity3;
}

bool axesFromLookAndNod(const Mat3& level, const Mat3& nodded, Mat3& axes) {
    //  The device's orientation D is the head's H times the device's place on it: D = H M^T, with M's rows the
    //  head's axes in the device's frame. Level, the head's up is the world's: up in the device's frame is D^T z.
    //  The nod turns the head about its left axis, nose down; D_level^T D_nodded is that turn seen from the device,
    //  so its axis, taken with a positive angle, is the head's left in the device's frame.
    const Vec3 up = unit(bambi::apply(transposed(level), Vec3{0, 0, 1}));
    const Mat3 turn = multiply(transposed(level), nodded);
    const double angle = std::acos(std::clamp((turn[0] + turn[4] + turn[8] - 1.0) / 2.0, -1.0, 1.0));
    Vec3 left{turn[7] - turn[5], turn[2] - turn[6], turn[3] - turn[1]};
    if (angle < 5.0 * kDeg2Rad || dot(left, left) < 1e-12) return false;
    left = unit(left);
    const Vec3 inPlane = left - up * dot(left, up);  // in the level plane: a nod has no roll in it
    if (dot(inPlane, inPlane) < 0.5) return false;   // the turn was about the up axis: a turn of the head, not a nod
    left = unit(inPlane);
    const Vec3 front = cross(left, up);
    axes = {front.x, front.y, front.z, left.x, left.y, left.z, up.x, up.y, up.z};
    return true;
}

Mat3 headFromYpr(double yawDeg, double pitchDeg, double rollDeg) {
    return multiply(
        multiply(rotationAbout({0, 0, 1}, yawDeg * kDeg2Rad), rotationAbout({0, 1, 0}, -pitchDeg * kDeg2Rad)),
        rotationAbout({1, 0, 0}, rollDeg * kDeg2Rad));
}

void yprOf(const Mat3& head, double& yawDeg, double& pitchDeg, double& rollDeg) {
    // The front vector is the first column: yaw from its heading, pitch from its height; the roll is what is left
    // once yaw and pitch are taken off.
    yawDeg = std::atan2(head[3], head[0]) * kRad2Deg;
    pitchDeg = std::asin(std::clamp(head[6], -1.0, 1.0)) * kRad2Deg;
    const Mat3 rest = multiply(transposed(headFromYpr(yawDeg, pitchDeg, 0.0)), head);  // Rx(roll)
    rollDeg = std::atan2(rest[7], rest[4]) * kRad2Deg;
}

double yawDegOf(const Mat3& head) { return std::atan2(head[3], head[0]) * kRad2Deg; }

bool headFromValues(const OrientationRead& read, std::span<const float> values, Mat3& out) {
    const bool quaternion =
        read.form == OrientationForm::QuaternionWxyz || read.form == OrientationForm::QuaternionXyzw;
    const std::size_t needed = quaternion ? 4 : 3;
    if (values.size() < needed) return false;
    for (std::size_t i = 0; i < needed; ++i)
        if (!std::isfinite(values[i])) return false;

    Mat3 device;
    if (quaternion) {
        double w = values[0], x = values[1], y = values[2], z = values[3];
        if (read.form == OrientationForm::QuaternionXyzw) {
            w = values[3];
            x = values[0];
            y = values[1];
            z = values[2];
        }
        const double n = std::sqrt(w * w + x * x + y * y + z * z);
        if (!(n > 0)) return false;
        device = fromQuaternion(w / n, (read.flip[0] ? -x : x) / n, (read.flip[1] ? -y : y) / n,
                                (read.flip[2] ? -z : z) / n);
    } else {
        const double scale = read.form == OrientationForm::YprRadians ? kRad2Deg : 1.0;
        const double down = read.form == OrientationForm::YprDegreesPitchDown ? -1.0 : 1.0;
        device = headFromYpr((read.flip[0] ? -1.0 : 1.0) * values[0] * scale,
                             (read.flip[1] ? -1.0 : 1.0) * down * values[1] * scale,
                             (read.flip[2] ? -1.0 : 1.0) * values[2] * scale);
    }
    out = multiply(device, transposed(axesOfMounting(read.mounting, read.axes)));
    return true;
}

Mat3 recentred(const Mat3& head, double yawDeg) { return multiply(rotationAbout({0, 0, 1}, -yawDeg * kDeg2Rad), head); }

bool knownForm(std::string_view address, OrientationForm& out) {
    if (address == "/adm/lis/ypr" || address == "/bambi/phone/ypr")  // the phone's page, through the helper
        out = OrientationForm::YprDegrees;
    else if (address == "/ypr" || address == "/SceneRotator/ypr")
        out = OrientationForm::YprDegreesPitchDown;
    else if (address == "/quaternion" || address == "/SceneRotator/quaternions")
        out = OrientationForm::QuaternionWxyz;
    else if (address == "/rotationvector")
        out = OrientationForm::QuaternionXyzw;
    else
        return false;
    return true;
}

}  // namespace bambi
