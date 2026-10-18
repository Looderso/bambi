// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/DecoderOverlay.h"

#include <cmath>

#include "bambi/math/sphere.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Strokes.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace sc = theme::scene;
namespace stroke = theme::stroke;
namespace shape = theme::shape;

constexpr double kGazeRingDeg = 9.0;   ///< the angular radius of the ring round where the head looks
constexpr double kEyeAngleRad = 0.45;  ///< each eye this far off the gaze, on the head's rim
constexpr int kRingSteps = 48;

/// `count` points of the small circle of angular radius `radiusRad` about `centre`.
void circleAround(Vec3 centre, double radiusRad, int count, std::vector<Vec3>& out) {
    const Vec3 c = centre * (1.0 / std::sqrt(dot(centre, centre)));
    const Vec3 a = std::abs(c.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    Vec3 u = cross(c, a);
    u = u * (1.0 / std::sqrt(dot(u, u)));
    const Vec3 v = cross(c, u);
    out.clear();
    for (int i = 0; i <= count; ++i) {
        const double f = 2.0 * kPi * i / count;
        out.push_back(c * std::cos(radiusRad) + (u * std::cos(f) + v * std::sin(f)) * std::sin(radiusRad));
    }
}
}  // namespace

void paintLoudspeakers(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                       std::span<const SceneSpeaker> speakers, float alpha) {
    const float side = sc::toggleBox;
    for (const SceneSpeaker& s : speakers) {
        const auto p = toScreen(projection, camera, vp, s.direction);
        const float a = alpha * (p.depth < 0.0 ? sc::backAlpha : 1.0f);
        const juce::Rectangle<float> box{static_cast<float>(p.x) - side / 2.0f, static_cast<float>(p.y) - side / 2.0f,
                                         side, side};
        g.setColour(colour::text.withAlpha(a));
        if (s.imaginary) {
            juce::Path outline;
            outline.addRectangle(box);
            strokeDashed(g, outline, stroke::unplacedDash, juce::PathStrokeType(stroke::toggle));
        } else {
            g.drawRect(box, stroke::toggle);
            text(g, juce::String(s.channel), smallFont(), colour::label.withAlpha(a),
                 {box.getRight() + 2.0f, box.getY() - side, 3.0f * side, side}, juce::Justification::centredLeft);
        }
    }
}

void paintHead(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp, Vec3 gaze, Vec3 left,
               float alpha) {
    std::vector<Vec3> ring;
    std::vector<ScreenSegment> segments;
    std::vector<juce::Point<double>> skipped;
    circleAround(gaze, kGazeRingDeg * kDeg2Rad, kRingSteps, ring);
    projectPolyline(projection, camera, vp, ring, true, segments);
    strokeSegments(g, segments, skipped, colour::text.withAlpha(alpha), stroke::regionEdge, false);
    const auto tip = toScreen(projection, camera, vp, gaze);
    if (projection == Projection::Globe) {
        //  the head itself at the centre: a circle with an eye on its rim either side of the gaze, kEyeAngleRad off
        //  it. Each eye is a direction scaled to the head's radius before projection, so the pair foreshortens as
        //  the head turns toward or away from the viewer, and tilts with roll.
        const float r = shape::sourceDot + sc::headRadiusPad;
        const double unit = static_cast<double>(r) / vp.rx;
        const double c = std::cos(kEyeAngleRad), sn = std::sin(kEyeAngleRad);
        const Vec3 leftEye{gaze.x * c + left.x * sn, gaze.y * c + left.y * sn, gaze.z * c + left.z * sn};
        const Vec3 rightEye{gaze.x * c - left.x * sn, gaze.y * c - left.y * sn, gaze.z * c - left.z * sn};
        const auto eyeL = toScreen(projection, camera, vp, leftEye * unit);
        const auto eyeR = toScreen(projection, camera, vp, rightEye * unit);
        g.setColour(colour::text.withAlpha(alpha));
        juce::Path line;
        line.startNewSubPath(static_cast<float>(vp.cx), static_cast<float>(vp.cy));
        line.lineTo(static_cast<float>(tip.x), static_cast<float>(tip.y));
        strokeDashed(g, line, stroke::otherPathDash, juce::PathStrokeType(stroke::rule));
        g.setColour(colour::ground.withAlpha(alpha));
        g.fillEllipse(static_cast<float>(vp.cx) - r, static_cast<float>(vp.cy) - r, 2.0f * r, 2.0f * r);
        g.setColour(colour::text.withAlpha(alpha));
        g.drawEllipse(static_cast<float>(vp.cx) - r, static_cast<float>(vp.cy) - r, 2.0f * r, 2.0f * r, stroke::toggle);
        const float eye = sc::eyeRadius;
        for (const auto& e : {eyeL, eyeR})
            g.fillEllipse(static_cast<float>(e.x) - eye, static_cast<float>(e.y) - eye, 2.0f * eye, 2.0f * eye);
    } else {
        g.setColour(colour::text.withAlpha(alpha * (tip.depth < 0.0 ? sc::backAlpha : 1.0f)));
        const float c = shape::sourceDot;
        g.drawLine(static_cast<float>(tip.x) - c, static_cast<float>(tip.y), static_cast<float>(tip.x) + c,
                   static_cast<float>(tip.y), stroke::toggle);
        g.drawLine(static_cast<float>(tip.x), static_cast<float>(tip.y) - c, static_cast<float>(tip.x),
                   static_cast<float>(tip.y) + c, stroke::toggle);
    }
}

std::vector<OverlayRow> decoderRows(const LinkScene& scene) {
    std::vector<OverlayRow> rows;
    for (const auto& entry : scene.entries()) {
        if (entry.product != Product::Decoder) continue;
        rows.push_back({{entry.instance, 0}, juce::String(entry.label()), colour::otherInstance, false});
        rows.push_back({{entry.instance, kDecoderPartSpeakers}, "speakers", colour::otherInstance, true});
        rows.push_back({{entry.instance, kDecoderPartHead}, "head", colour::otherInstance, true});
    }
    return rows;
}

DecoderOverlayDrawn paintDecoderOverlay(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                         const LinkScene& scene, const OverlayMenu& menu) {
    DecoderOverlayDrawn drawn;
    std::vector<SceneSpeaker> speakers;
    for (const auto& entry : scene.entries()) {
        if (entry.product != Product::Decoder || !menu.drawn({entry.instance, 0})) continue;

        //  the layout travels with the instance while it plays one: real and imaginary points
        if (menu.drawn({entry.instance, kDecoderPartSpeakers}) && (entry.dyn.playFlags & kLinkPlaySpeakers) != 0) {
            speakers.clear();
            const auto count = std::min<std::size_t>(entry.st.speakerCount, kLinkMaxSpeakers);
            for (std::size_t i = 0; i < count; ++i) {
                const LinkSpeaker& s = entry.st.speakers[i];
                speakers.push_back({fromAzEl(static_cast<double>(s.azimuthDeg) * kDeg2Rad,
                                             static_cast<double>(s.elevationDeg) * kDeg2Rad),
                                    s.channel, s.imaginary != 0});
            }
            paintLoudspeakers(g, projection, camera, vp, speakers, 1.0f);
            drawn.speakers += static_cast<int>(speakers.size());
        }
        //  the head as the Decoder has it this block, in the room's frame
        if (menu.drawn({entry.instance, kDecoderPartHead}) && (entry.dyn.playFlags & kLinkPlayHeadphones) != 0) {
            paintHead(g, projection, camera, vp, entry.dyn.headGaze(), entry.dyn.headLeft(), 1.0f);
            ++drawn.heads;
            drawn.gaze = entry.dyn.headGaze();
        }
    }
    return drawn;
}

}  // namespace bambi::ui
