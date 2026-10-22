// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneView.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "bambi/decode/orientation.hpp"
#include "bambi/math/sphere.hpp"
#include "bambi/ui/DecoderOverlay.h"
#include "bambi/ui/Draw.h"
#include "bambi/ui/OverlayMenu.h"
#include "bambi/ui/Strokes.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

using namespace theme;
namespace sc = theme::scene;

namespace {
constexpr int kArcSteps = 16;
}  // namespace

DecoderSceneView::DecoderSceneView(DecoderControlState& state, Projection projection)
    : state_(state), projection_(projection) {
    setOpaque(true);
}

Vec3 DecoderSceneView::toView(Vec3 p) const { return state_.headFrame ? bambi::apply(transposed(head_), p) : p; }

void DecoderSceneView::refreshDesign() {
    const auto& layout = state_.decoder().layout;
    if (layout == designOf_ && (!design_.points.empty() || layout.empty())) return;
    designOf_ = layout;
    design_ = layout.empty() ? SpeakerDesign{} : state_.processor.speakerDesign();
}

void DecoderSceneView::paint(juce::Graphics& g) {
    clearRegions();
    const bool globe = projection_ == Projection::Globe;
    g.fillAll(colour::panel);

    //  the caption, the globe's view presets and the equirect's corner: every scene view's
    strip_ = paintSceneStrip(g, getWidth(), projection_, state_.sphere, false);  // no regions on this scene
    addSceneStripClicks(*this, strip_, state_.sphere, relayout_, [this] { repaint(); });
    //  and the decoder's own two, one a strip: the frame the scene is drawn in beside the equirect's switches, the
    //  sources beside the globe's presets
    const juce::Rectangle<float> row{0.0f, 0.0f, static_cast<float>(getWidth()), static_cast<float>(sc::labelStrip)};
    if (!globe) {
        const float right = strip_.full.getX() - sc::toggleGroupGap;  // the strip lays its toggles out from the right
        const auto frame = paintSceneToggle(g, row, right, "head frame", state_.headFrame);
        headFrameToggle_ = frame.hit;
        addClickArea(frame.hit, [this] {
            state_.headFrame = !state_.headFrame;
            state_.notify();
        });
    } else {
        const float right = strip_.presets[0].getX() - sc::toggleGroupGap;
        state_.sources.paintToggle(g, row, right);
    }

    const auto& diag = state_.processor.diagnostics();
    head_ = headFromYpr(diag.headYaw.load(std::memory_order_relaxed), diag.headPitch.load(std::memory_order_relaxed),
                        diag.headRoll.load(std::memory_order_relaxed));
    scene_ = headFromYpr(diag.sceneYaw.load(std::memory_order_relaxed), diag.scenePitch.load(std::memory_order_relaxed),
                         diag.sceneRoll.load(std::memory_order_relaxed));
    refreshDesign();

    const auto vp = viewport();
    const auto frame = sceneFrame(vp);
    {
        juce::Graphics::ScopedSaveState clip(g);
        if (!globe) g.reduceClipRegion(frame.getSmallestIntegerContainer());
        //  the energy of what every output hears, under everything drawn in ink, turned with the gaze as they are
        if (state_.sphere.energyOn && state_.sphere.energyAvailable)
            energyLayer_.paint(g, projection_, state_.sphere.camera, vp, state_.sphere.energy, 1.0f,
                               state_.headFrame ? &head_ : nullptr);
        paintGraticule(g, vp);

        //  What plays decides what is full ink.
        const DecoderState& d = state_.decoder();
        const float back = sc::backAlpha;
        const float headAlpha = d.headphonesPlay ? 1.0f : back;
        const float speakerAlpha = d.speakersPlay || (d.headphonesPlay && d.throughLayout) ? 1.0f : back;
        if (state_.page == OutputPage::Speakers) paintHull(g, vp, 1.0f);
        paintSources(g, vp);
        paintSpeakers(g, vp, speakerAlpha);
        paintHead(g, vp, headAlpha);
    }
    paintSceneFrame(g, vp, projection_);

    //  the equirect's letters: back, left, front (or the gaze), right, back
    if (!globe) {
        const char* letters[5] = {"b", "l", state_.headFrame ? "gaze" : "f", "r", "b"};
        const auto y = static_cast<float>(vp.cy - vp.ry) - sc::axisLabelGap;
        for (int i = 0; i < 5; ++i) {
            const auto x = static_cast<float>(vp.screenX(-1.0 + 0.5 * i));
            text(g, letters[i], smallFont(), colour::axisLabel,
                 juce::Rectangle<float>(0.0f, 0.0f, 2.0f * sc::labelStrip, static_cast<float>(sc::labelStrip))
                     .withCentre({x, y}),
                 juce::Justification::centred);
        }
    }

    //  A drag orbits the globe. Regions added later lie on top, so this one is added before the list's, which must take
    //  a press over the globe and never let it start an orbit.
    if (globe) {
        addDragArea(
            frame, [this](juce::Point<float> at) { dragLast_ = at; },
            [this](juce::Point<float> at, bool) {
                orbitView(state_.sphere, static_cast<double>(at.x - dragLast_.x),
                          static_cast<double>(at.y - dragLast_.y));
                dragLast_ = at;
                state_.notify();
            },
            [] {});
        //  the list over everything, last
        state_.sources.paintList(g, static_cast<float>(sc::labelStrip));
        state_.sources.addClicks(*this, getLocalBounds().toFloat(), [this] { state_.notify(); });
    }
}

void DecoderSceneView::mouseMove(const juce::MouseEvent& e) {
    if (state_.sources.mouseMove(e.position)) state_.notify();
}

void DecoderSceneView::mouseExit(const juce::MouseEvent&) {
    if (state_.sources.mouseExit()) state_.notify();
}

bool DecoderSceneView::keyPressed(const juce::KeyPress& key) {
    if (!state_.sources.keyPressed(key)) return false;
    state_.notify();
    return true;
}

void DecoderSceneView::paintGraticule(juce::Graphics& g, const Viewport& vp) {
    for (const bool major : {false, true}) {
        segments_.clear();
        for (const auto& line : graticule()) {
            if (line.major != major) continue;
            lineSegments_.clear();
            arc_.clear();
            for (const Vec3& p : line.points) arc_.push_back(toView(p));
            projectPolyline(projection_, state_.sphere.camera, vp, arc_, false, lineSegments_);
            segments_.insert(segments_.end(), lineSegments_.begin(), lineSegments_.end());
        }
        strokeSegments(g, segments_, skipped_, major ? colour::graticuleMain : colour::graticule, stroke::graticule,
                       false);
    }
}

void DecoderSceneView::strokeArc(juce::Graphics& g, const Viewport& vp, Vec3 a, Vec3 b, juce::Colour colour,
                                 float width, bool dashed) {
    arc_.clear();
    for (int i = 0; i <= kArcSteps; ++i) arc_.push_back(toView(slerp(a, b, static_cast<double>(i) / kArcSteps)));
    segments_.clear();
    projectPolyline(projection_, state_.sphere.camera, vp, arc_, false, segments_);
    strokeSegments(g, segments_, skipped_, colour, width, dashed);
}

void DecoderSceneView::paintHull(juce::Graphics& g, const Viewport& vp, float alpha) {
    if (design_.points.empty() || design_.triangles.empty()) return;
    std::set<std::pair<int, int>> edges;
    for (const auto& t : design_.triangles)
        for (const auto [i, j] : {std::pair{t[0], t[1]}, std::pair{t[1], t[2]}, std::pair{t[0], t[2]}})
            edges.insert({std::min(i, j), std::max(i, j)});
    for (const auto& [i, j] : edges) {
        if (i < 0 || j < 0 || i >= static_cast<int>(design_.points.size()) ||
            j >= static_cast<int>(design_.points.size()))
            continue;
        const bool imaginary =
            design_.imaginary[static_cast<std::size_t>(i)] || design_.imaginary[static_cast<std::size_t>(j)];
        strokeArc(g, vp, design_.points[static_cast<std::size_t>(i)], design_.points[static_cast<std::size_t>(j)],
                  colour::sphere.withAlpha(alpha * 0.5f), stroke::graticule, imaginary);
    }
}

void DecoderSceneView::paintSpeakers(juce::Graphics& g, const Viewport& vp, float alpha) {
    speakers_.clear();
    for (const Loudspeaker& s : state_.decoder().layout)
        speakers_.push_back(
            {toView(fromAzEl(s.azimuthDeg * kDeg2Rad, s.elevationDeg * kDeg2Rad)), s.channel, s.imaginary});
    paintLoudspeakers(g, projection_, state_.sphere.camera, vp, speakers_, alpha);
}

void DecoderSceneView::paintSources(juce::Graphics& g, const Viewport& vp) {
    sourcesDrawn_ = 0;
    for (const auto& entry : state_.scene.entries()) {
        if (!entry.hasPosition || entry.product == Product::Decoder) continue;
        const OverlayKey key{entry.instance, 0};
        if (!state_.sources.drawn(key)) continue;
        ++sourcesDrawn_;
        //  where the source is heard: turned with the scene
        const auto p =
            toScreen(projection_, state_.sphere.camera, vp, toView(bambi::apply(scene_, entry.dyn.position())));
        const float a = p.depth < 0.0 ? sc::backAlpha : 1.0f;
        const float r = shape::otherSourceDot;
        g.setColour(colour::otherInstance.withAlpha(a));
        g.fillEllipse(static_cast<float>(p.x) - r, static_cast<float>(p.y) - r, 2.0f * r, 2.0f * r);
        //  the row under the pointer in the list, pointed at
        if (state_.sources.hovered(key)) {
            const float ring = r + sc::hoverRing;
            g.setColour(colour::touched.withAlpha(a));
            g.drawEllipse(static_cast<float>(p.x) - ring, static_cast<float>(p.y) - ring, 2.0f * ring, 2.0f * ring,
                          stroke::rule);
        }
        text(g, juce::String(entry.label()), smallFont(), colour::label.withAlpha(a),
             {static_cast<float>(p.x) + r + 3.0f, static_cast<float>(p.y) - r - 2.0f, sc::otherSourceLabelWidth,
              2.0f * r + 4.0f},
             juce::Justification::centredLeft);
    }
}

void DecoderSceneView::paintHead(juce::Graphics& g, const Viewport& vp, float alpha) {
    const Vec3 gaze = state_.headFrame
                          ? Vec3{1, 0, 0}
                          : bambi::apply(head_, Vec3{1, 0, 0});  // bambi::, not std::apply through the array
    const Vec3 left = state_.headFrame ? Vec3{0, 1, 0} : bambi::apply(head_, Vec3{0, 1, 0});
    bambi::ui::paintHead(g, projection_, state_.sphere.camera, vp, gaze, left, alpha);
}

}  // namespace bambi::ui
