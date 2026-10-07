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

/// Rows: the head's front, left and up, in the device's coordinates (a phone's: x right, y top edge, z out of the
/// screen). The device's rotation times this transposed is the head's.
Mat3 axesOf(Mounting mounting) {
    switch (mounting) {
        case Mounting::PhoneTopEdge: return {0, 1, 0, -1, 0, 0, 0, 0, 1};
        case Mounting::PhoneBack: return {0, 0, -1, -1, 0, 0, 0, 1, 0};
        case Mounting::AsSent: break;
    }
    return kIdentity3;
}
}  // namespace

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
    out = multiply(device, transposed(axesOf(read.mounting)));
    return true;
}

Mat3 recentred(const Mat3& head, double yawDeg) { return multiply(rotationAbout({0, 0, 1}, -yawDeg * kDeg2Rad), head); }

bool knownForm(std::string_view address, OrientationForm& out) {
    if (address == "/adm/lis/ypr")
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
