// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <vector>

#include "bambi/decode/orientation.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
/// The angle between two orientations, degrees.
double apart(const Mat3& a, const Mat3& b) {
    const Mat3 r = multiply(a, transposed(b));
    return std::acos(std::clamp((r[0] + r[4] + r[8] - 1.0) / 2.0, -1.0, 1.0)) * kRad2Deg;
}

/// The bridge's quaternion for the suite's angles: Rz(yaw) Ry(-pitch) Rx(roll) as w, x, y, z, from half-angles --
/// written here as the prototype's osc-bridge.py writes it, not through the matrices under test.
std::array<float, 4> bridgeQuaternion(double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw * kDeg2Rad / 2), sy = std::sin(yaw * kDeg2Rad / 2);
    const double cp = std::cos(-pitch * kDeg2Rad / 2), sp = std::sin(-pitch * kDeg2Rad / 2);
    const double cr = std::cos(roll * kDeg2Rad / 2), sr = std::sin(roll * kDeg2Rad / 2);
    return {static_cast<float>(cy * cp * cr + sy * sp * sr), static_cast<float>(cy * cp * sr - sy * sp * cr),
            static_cast<float>(cy * sp * cr + sy * cp * sr), static_cast<float>(sy * cp * cr - cy * sp * sr)};
}

Mat3 read(OrientationForm form, std::vector<float> v, Mounting m = Mounting::AsSent, std::array<bool, 3> flip = {}) {
    OrientationRead r;
    r.form = form;
    r.mounting = m;
    r.flip = flip;
    Mat3 out = kIdentity3;
    REQUIRE(headFromValues(r, v, out));
    return out;
}
}  // namespace

TEST_CASE("the head's angles follow the suite's convention and come back from the matrix") {
    //  Catches: a yaw sign, pitch about the wrong axis, roll about the wrong one; and a readback that loses a sign.
    CHECK(apart(headFromYpr(90, 0, 0), rotationAbout({0, 0, 1}, 90.0 * kDeg2Rad)) < 1e-9);  // left is +z
    const Mat3 up = headFromYpr(0, 30, 0);
    CHECK(apply(up, {1, 0, 0}).z == doctest::Approx(std::sin(30.0 * kDeg2Rad)));  // looking up lifts the front
    const Mat3 roll = headFromYpr(0, 0, 20);
    CHECK(apply(roll, {0, 1, 0}).z == doctest::Approx(std::sin(20.0 * kDeg2Rad)));  // the left ear goes up
    for (const auto [y, p, r] :
         {std::array{40.0, 25.0, -15.0}, std::array{-170.0, -60.0, 100.0}, std::array{0.0, 0.0, 0.0}}) {
        double yy, pp, rr;
        yprOf(headFromYpr(y, p, r), yy, pp, rr);
        CHECK(yy == doctest::Approx(y).epsilon(1e-9));
        CHECK(pp == doctest::Approx(p).epsilon(1e-9));
        CHECK(rr == doctest::Approx(r).epsilon(1e-9));
    }
}

TEST_CASE("every format reads back the orientation it was sent, to 0.01 degrees") {
    //  Catches: the pitch sign of IEM and SPARTA, the w position of a quaternion, radians read as degrees.
    const double yaw = 37.0, pitch = -22.0, roll = 11.0;
    const Mat3 want = headFromYpr(yaw, pitch, roll);
    const std::array<float, 4> q = bridgeQuaternion(yaw, pitch, roll);
    const float yf = static_cast<float>(yaw), pf = static_cast<float>(pitch), rf = static_cast<float>(roll);
    CHECK(apart(read(OrientationForm::YprDegrees, {yf, pf, rf}), want) < 0.01);                  // ADM-OSC
    CHECK(apart(read(OrientationForm::YprDegreesPitchDown, {yf, -pf, rf}), want) < 0.01);        // SPARTA, IEM
    CHECK(apart(read(OrientationForm::QuaternionWxyz, {q[0], q[1], q[2], q[3]}), want) < 0.01);  // SPARTA, IEM, AirPods
    CHECK(apart(read(OrientationForm::QuaternionXyzw, {q[1], q[2], q[3], q[0]}), want) < 0.01);  // Android
    CHECK(apart(read(OrientationForm::YprRadians,
                     {static_cast<float>(yaw * kDeg2Rad), static_cast<float>(pitch * kDeg2Rad),
                      static_cast<float>(roll * kDeg2Rad)}),
                want) < 0.01);
    // A quaternion twice as long reads the same; one that is not a number, or too short, is refused.
    CHECK(apart(read(OrientationForm::QuaternionWxyz, {2 * q[0], 2 * q[1], 2 * q[2], 2 * q[3]}), want) < 0.01);
    OrientationRead r;
    Mat3 out = kIdentity3;
    CHECK_FALSE(headFromValues(r, std::vector<float>{1.0f, 0.0f, 0.0f}, out));
    CHECK_FALSE(headFromValues(r, std::vector<float>{1.0f, 0.0f, 0.0f, std::nanf("")}, out));
    CHECK(out == kIdentity3);
}

TEST_CASE("learn mode's flips undo a sender's inverted axis") {
    //  Catches: a flip applied to the wrong component.
    const Mat3 want = headFromYpr(30, 20, -10);
    CHECK(apart(read(OrientationForm::YprDegrees, {30, -20, -10}, Mounting::AsSent, {false, true, false}), want) <
          1e-9);
    CHECK(apart(read(OrientationForm::YprDegrees, {-30, 20, 10}, Mounting::AsSent, {true, false, true}), want) < 1e-9);
    const std::array<float, 4> q = bridgeQuaternion(30, 20, -10);
    CHECK(
        apart(read(OrientationForm::QuaternionWxyz, {q[0], q[1], -q[2], q[3]}, Mounting::AsSent, {false, true, false}),
              want) < 1e-6);
}

TEST_CASE("a phone on the band or held in front maps into the head's frame") {
    //  Catches: a mounting matrix transposed, or the wrong device axis called the front or the up.
    //  A phone's sensors give its rotation in the world, with the phone flat and its top edge along world +y as the
    //  identity; the device's axes are x right, y top edge, z out of the screen.
    auto aboutX = [](double deg) {  // the device tipped about its own x, as a quaternion w x y z
        const double h = deg * kDeg2Rad / 2;
        return std::vector<float>{static_cast<float>(std::cos(h)), static_cast<float>(std::sin(h)), 0.0f, 0.0f};
    };
    double y, p, r;
    // On the band, screen up, top edge forward: the flat phone is a level head whose front is world +y.
    yprOf(read(OrientationForm::QuaternionWxyz, {1, 0, 0, 0}, Mounting::PhoneTopEdge), y, p, r);
    CHECK(y == doctest::Approx(90.0));
    CHECK(std::abs(p) < 1e-9);
    CHECK(std::abs(r) < 1e-9);
    // Nodding down lowers the top edge: the phone tips about its x by -20 degrees; the head pitches down 20.
    yprOf(read(OrientationForm::QuaternionWxyz, aboutX(-20), Mounting::PhoneTopEdge), y, p, r);
    CHECK(p == doctest::Approx(-20.0).epsilon(1e-6));
    CHECK(std::abs(r) < 1e-6);
    // Held upright in front, screen facing the listener: the flat phone tipped up by 90 about its x. The screen then
    // faces world -y, the listener looks toward +y, level.
    yprOf(read(OrientationForm::QuaternionWxyz, aboutX(90), Mounting::PhoneBack), y, p, r);
    CHECK(y == doctest::Approx(90.0));
    CHECK(std::abs(p) < 1e-9);
    CHECK(std::abs(r) < 1e-9);
    // Tipping its top edge away from the listener, 20 degrees past upright, is the head looking up 20.
    yprOf(read(OrientationForm::QuaternionWxyz, aboutX(110), Mounting::PhoneBack), y, p, r);
    CHECK(p == doctest::Approx(20.0).epsilon(1e-6));
    CHECK(std::abs(r) < 1e-6);
    // Turning the upright phone about the vertical turns the head the same way: 30 degrees left.
    const Mat3 turned = multiply(rotationAbout({0, 0, 1}, 30.0 * kDeg2Rad), rotationAbout({1, 0, 0}, 90.0 * kDeg2Rad));
    OrientationRead rd;
    rd.form = OrientationForm::YprDegrees;
    rd.mounting = Mounting::PhoneBack;
    double ty, tp, tr;
    yprOf(turned, ty, tp, tr);  // the device's rotation, as the suite's angles about the same world axes
    Mat3 head;
    REQUIRE(headFromValues(
        rd, std::vector<float>{static_cast<float>(ty), static_cast<float>(tp), static_cast<float>(tr)}, head));
    yprOf(head, y, p, r);
    CHECK(y == doctest::Approx(120.0).epsilon(1e-6));
    CHECK(std::abs(p) < 1e-6);
}

TEST_CASE("recentring takes off the yaw only: pitch and roll stay against gravity") {
    //  Catches: recentring by the whole inverse orientation, which adds a false roll to a plain turn.
    const Mat3 atRecentre = headFromYpr(40, -30, 0);  // turned right, looking down
    const double yaw0 = yawDegOf(atRecentre);
    CHECK(yaw0 == doctest::Approx(40.0));
    const Mat3 later = recentred(headFromYpr(40 + 25, -30, 0), yaw0);
    double y, p, r;
    yprOf(later, y, p, r);
    CHECK(y == doctest::Approx(25.0).epsilon(1e-9));
    CHECK(p == doctest::Approx(-30.0).epsilon(1e-9));
    CHECK(std::abs(r) < 1e-9);  // a plain turn stays a plain turn
    // Taking off the whole orientation instead would roll the head on the same turn.
    const Mat3 wrong = multiply(transposed(atRecentre), headFromYpr(65, -30, 0));
    yprOf(wrong, y, p, r);
    CHECK(std::abs(r) > 5.0);
}

TEST_CASE("the well-known addresses name their form; anything else goes to learn") {
    //  Catches: an address mapped to the other family's pitch sign.
    OrientationForm f;
    REQUIRE(knownForm("/adm/lis/ypr", f));
    CHECK(f == OrientationForm::YprDegrees);
    REQUIRE(knownForm("/ypr", f));
    CHECK(f == OrientationForm::YprDegreesPitchDown);
    REQUIRE(knownForm("/SceneRotator/ypr", f));
    CHECK(f == OrientationForm::YprDegreesPitchDown);
    REQUIRE(knownForm("/quaternion", f));
    CHECK(f == OrientationForm::QuaternionWxyz);
    REQUIRE(knownForm("/SceneRotator/quaternions", f));
    CHECK(f == OrientationForm::QuaternionWxyz);
    REQUIRE(knownForm("/rotationvector", f));
    CHECK(f == OrientationForm::QuaternionXyzw);
    CHECK_FALSE(knownForm("/gyrosc/quat", f));
}

TEST_CASE("AirPods report the headphone frame: level on the head is a level head, a nod is a nod") {
    //  Catches: the headphones' frame mapped as a tracker's own, which would put the front at the top of the head.
    double y, p, r;
    //  Apple's frame on the head: x right, y up, z back. Worn level, facing world +y as the arbitrary x reference
    //  has it, the attitude is the same as an upright phone facing the listener.
    const auto aboutX = [](double deg) {
        const double h = deg * kDeg2Rad / 2;
        return std::vector<float>{static_cast<float>(std::cos(h)), static_cast<float>(std::sin(h)), 0.0f, 0.0f};
    };
    yprOf(read(OrientationForm::QuaternionWxyz, aboutX(90), Mounting::AirPods), y, p, r);
    CHECK(std::abs(p) < 1e-9);
    CHECK(std::abs(r) < 1e-9);
    yprOf(read(OrientationForm::QuaternionWxyz, aboutX(110), Mounting::AirPods), y, p, r);
    CHECK(p == doctest::Approx(20.0).epsilon(1e-6));  // the face tipped up by 20 degrees
}
