// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/ProbeView.h"

#include <array>
#include <cmath>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Strokes.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;
namespace sc = theme::scene;

//  the panel around the sphere is the encoder's, exactly: a caption strip, the sphere's outline or
//  the equirect's frame, and the axis letters, so every plugin reads as one program
struct Anchor {
    double azDeg;
    const char* name;
};
constexpr std::array<Anchor, 4> kAnchors{{{0.0, "f"}, {90.0, "l"}, {180.0, "b"}, {270.0, "r"}}};
constexpr std::array<const char*, 5> kEquirectLetters{"b", "l", "f", "r", "b"};

//  four taps want four colours, and the palette already spends orange on what arrived and blue on
//  what was added, so these step around that blue rather than away from it -- all one family, none
//  of them the wash's own colour at full strength. The number that labels a spiral is the number in
//  the strip and in the selector. An answer that is not numbered is one thing the plugin adds, and takes
//  the added colour
juce::Colour answerInk(const ProbeReply& reply, int group) {
    if (!reply.numbered) return colour::added;
    const auto& family = colour::answerFamily;
    return family[static_cast<std::size_t>(std::clamp(group, 0, static_cast<int>(family.size()) - 1))];
}
}  // namespace

ProbeView::ProbeView(State& state, Projection projection, ProbeAnswer answer, std::function<void()> changed)
    : state_(state), projection_(projection), answer_(std::move(answer)), changed_(std::move(changed)) {
    setOpaque(true);  // it fills its own panel, as the encoder's scene does
}

//  The encoder's, so a sphere is the same size and in the same place in every plugin.
Viewport ProbeView::viewport() const { return sceneViewport(projection_, getWidth(), getHeight()); }

/// The caption, the outline or the frame, and the axis letters: the encoder's panel, shared.
void ProbeView::paintFrame(juce::Graphics& g, const Viewport& vp) {
    const bool globe = projection_ == Projection::Globe;

    g.fillAll(colour::panel);
    //  the caption, the globe's view presets and the equirect's corner: every scene view's
    strip_ = paintSceneStrip(g, getWidth(), projection_, state_);
    regionsToggle_ = strip_.regions;
    energyToggle_ = strip_.energy;
    fullToggle_ = strip_.full;

    const juce::Rectangle<float> frame{static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry),
                                       static_cast<float>(2.0 * vp.rx), static_cast<float>(2.0 * vp.ry)};
    g.setColour(colour::sphere);
    if (globe)
        g.drawEllipse(frame, theme::stroke::rule);
    else
        g.drawRect(frame, theme::stroke::rule);
}

void ProbeView::paintAxisLabels(juce::Graphics& g, const Viewport& vp) {
    const auto strip = static_cast<float>(sc::labelStrip);
    if (projection_ == Projection::Globe) {
        for (const auto& anchor : kAnchors) {
            const auto s = toScreen(projection_, state_.camera, vp, fromAzEl(anchor.azDeg * kDeg2Rad, 0.0));
            if (s.depth <= 0.0) continue;
            text(g, anchor.name, smallFont(), colour::axisLabel,
                 juce::Rectangle<float>(0.0f, 0.0f, strip, strip)
                     .withCentre({static_cast<float>(s.x), static_cast<float>(s.y)}),
                 juce::Justification::centred);
        }
        return;
    }
    const auto y = static_cast<float>(vp.cy - vp.ry) - sc::axisLabelGap;
    for (std::size_t i = 0; i < kEquirectLetters.size(); ++i) {
        const auto x = static_cast<float>(vp.screenX(-1.0 + 0.5 * static_cast<double>(i)));
        text(g, kEquirectLetters[i], smallFont(), colour::axisLabel,
             juce::Rectangle<float>(0.0f, 0.0f, strip, strip).withCentre({x, y}), juce::Justification::centred);
    }
}

juce::Point<float> ProbeView::pixelsFor(double nx, double ny) const {
    const auto vp = viewport();
    return {static_cast<float>(vp.screenX(nx)), static_cast<float>(vp.screenY(ny))};
}

bool ProbeView::placeAt(double nx, double ny) {
    const auto at = unproject(projection_, state_.camera, nx, ny);
    if (!at.has_value()) return false;
    state_.at = *at;
    state_.placed = true;
    if (changed_) changed_();
    return true;
}

namespace {
//  The globe's own texture, as the reference implementation sizes it.
}  // namespace

void ProbeView::paintEnergy(juce::Graphics& g, const Viewport& vp) {
    energyDrawn_ = false;
    if (!state_.energyOn || !state_.energyAvailable) return;
    //  half, while a region's tab is open: the wash under it is what is being edited, and against an
    //  undimmed peak it is not visible. It costs about 4 dB of the energy's range, and at 0.35 the
    //  orange starts to go
    const float opacity = openRegion_ >= 0 ? sc::energyUnderRegion : 1.0f;
    energyDrawn_ = energyLayer_.paint(g, projection_, state_.camera, vp, state_.energy, opacity);
}

//  In world space, drawn through whichever projection, so no view special-cases a seam.
void ProbeView::paintGraticule(juce::Graphics& g, const Viewport& vp) {
    //  the main lines, then the rest: each weight is gathered whole and stroked as the encoder's
    //  scene strokes a line -- a near path and a far one -- rather than a colour set and a line
    //  drawn for every segment, which is most of what a repaint costs
    for (const bool major : {false, true}) {
        segments_.clear();
        for (const auto& line : graticule()) {
            if (line.major != major) continue;
            lineSegments_.clear();
            projectPolyline(projection_, state_.camera, vp, line.points, false, lineSegments_);
            segments_.insert(segments_.end(), lineSegments_.begin(), lineSegments_.end());
        }
        strokeSegments(g, segments_, skipped_, major ? colour::graticuleMain : colour::graticule,
                       theme::stroke::graticule, false);
    }
}

/*  A path whose opacity changes along it: the fade of a loop is a gradient and there is no gradient
    along an arbitrary path, so the segments are sorted into a handful of opacity bands and each band
    strokes once. The far side of the globe is drawn too, dimmed, so a curve never breaks in half. */
void ProbeView::strokeFaded(juce::Graphics& g, const Viewport& vp, const ProbeTrace& trace) {
    const auto n = trace.points.size();
    if (n < 2) return;
    constexpr int kBands = 14;
    std::array<juce::Path, kBands> bands;

    const float lead = trace.lead ? 1.0f : 0.4f;
    const float scale = trace.faint ? sc::flowAlpha : 1.0f;
    ScreenPoint previous = toScreen(projection_, state_.camera, vp, trace.points[0]);
    for (std::size_t k = 1; k < n; ++k) {
        const auto here = toScreen(projection_, state_.camera, vp, trace.points[k]);
        //  the equirect's back seam: a segment across it is not a segment, it is the wrap
        const bool seam = projection_ == Projection::Equirect && std::abs(here.x - previous.x) > vp.rx;
        if (!seam) {
            const float w = trace.weight.empty() ? 1.0f : trace.weight[std::min(k, trace.weight.size() - 1)];
            const float dim = (previous.depth <= 0.0 || here.depth <= 0.0) ? sc::backAlpha : 1.0f;
            const float alpha = std::clamp(w * lead * scale * dim, 0.0f, 1.0f);
            if (alpha >= ctl::markFloor) {
                const int band = std::min(kBands - 1, static_cast<int>(alpha * kBands));
                bands[static_cast<std::size_t>(band)].startNewSubPath(static_cast<float>(previous.x),
                                                                      static_cast<float>(previous.y));
                bands[static_cast<std::size_t>(band)].lineTo(static_cast<float>(here.x), static_cast<float>(here.y));
            }
        }
        previous = here;
    }
    for (int band = 0; band < kBands; ++band) {
        if (bands[static_cast<std::size_t>(band)].isEmpty()) continue;
        g.setColour(answerInk(reply_, trace.group).withAlpha((static_cast<float>(band) + 0.5f) / kBands));
        g.strokePath(
            bands[static_cast<std::size_t>(band)],
            juce::PathStrokeType(theme::stroke::rule, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

/*  One pass, at the width of sky it arrives from: a soft edge that is that pass's blur, in degrees.
    The same energy spread wider is fainter, so the wash dims as it grows -- otherwise the late
    passes, which are the widest and the quietest, are the only thing left. The hard core keeps the
    pass locatable. */
void ProbeView::paintBead(juce::Graphics& g, const Viewport& vp, const ProbeMark& mark) {
    const auto at = toScreen(projection_, state_.camera, vp, mark.direction);
    const float dim = at.depth <= 0.0 ? sc::beadBackAlpha : 1.0f;
    const auto lead = mark.lead ? 1.0f : 0.4f;
    const auto weight = static_cast<float>(std::clamp(mark.weight, 0.0, 1.0));
    const float alpha =
        std::clamp(lead * (sc::beadFloor + (1.0f - sc::beadFloor) * std::pow(weight, ctl::markLevelPower)), 0.0f, 1.0f);
    if (alpha < ctl::markFloor) return;

    const double spreadDeg =
        std::clamp(mark.spreadRad * kRad2Deg, static_cast<double>(sc::beadMinDeg), static_cast<double>(sc::beadMaxDeg));
    const auto radius = static_cast<float>(projection_ == Projection::Globe ? vp.rx * std::sin(spreadDeg * kDeg2Rad)
                                                                            : vp.rx * spreadDeg / 180.0);
    const auto ink = answerInk(reply_, mark.group);

    //  the wash, dimmed as it widens
    const auto soft = alpha * dim / static_cast<float>(1.0 + spreadDeg / sc::beadSpreadDim);
    juce::ColourGradient gradient(ink.withAlpha(soft * sc::beadCoreAlpha), static_cast<float>(at.x),
                                  static_cast<float>(at.y), ink.withAlpha(0.0f), static_cast<float>(at.x) + radius,
                                  static_cast<float>(at.y), true);
    gradient.addColour(sc::beadMidStop, ink.withAlpha(soft * sc::beadMidAlpha));
    g.setGradientFill(gradient);
    g.fillEllipse(static_cast<float>(at.x) - radius, static_cast<float>(at.y) - radius, 2.0f * radius, 2.0f * radius);

    //  and the core, so the pass stays locatable however wide it has spread
    const auto core = theme::stroke::rule + sc::beadCore * alpha;
    g.setColour(ink.withAlpha(std::min(1.0f, alpha * 1.2f) * dim));
    g.fillEllipse(static_cast<float>(at.x) - core, static_cast<float>(at.y) - core, 2.0f * core, 2.0f * core);
}

//  what the plugin answers: the beads first and the thread over them, since the wash is context and
//  the line is the thing being read
void ProbeView::paintAnswer(juce::Graphics& g, const Viewport& vp) {
    reply_.marks.clear();
    reply_.traces.clear();
    reply_.numbered = true;
    if (answer_) answer_(state_.at, reply_);

    for (const auto& trace : reply_.traces)
        if (trace.faint) strokeFaded(g, vp, trace);  // the flow, under everything it explains
    for (const auto& mark : reply_.marks) paintBead(g, vp, mark);
    for (const auto& trace : reply_.traces)
        if (!trace.faint) strokeFaded(g, vp, trace);
}

//  the regions, over the energy and under the probe: ink over colour, so what a region is reads as
//  structure and the colour stays with what arrived and what was added. Only the open slot takes handles
void ProbeView::paintRegions(juce::Graphics& g, const Viewport& vp) {
    regionLayer_.paint(g, projection_, state_.camera, vp, regions_);
    grip_.paint(g, projection_, state_.camera, vp, regions_, openRegion_);
}

//  where it is pointed: until it has been placed it sits at the front with a dashed ring, so it
//  says it was never placed rather than looking like a choice
void ProbeView::paintProbe(juce::Graphics& g, const Viewport& vp) {
    const auto here = toScreen(projection_, state_.camera, vp, state_.at);
    const auto r = theme::shape::probeDot;
    g.setColour(colour::probe.withAlpha(here.depth > 0.0 ? 1.0f : sc::backAlpha));
    if (state_.placed) {
        g.fillEllipse(static_cast<float>(here.x) - r, static_cast<float>(here.y) - r, 2.0f * r, 2.0f * r);
        return;
    }
    juce::Path ring;
    ring.addEllipse(static_cast<float>(here.x) - r, static_cast<float>(here.y) - r, 2.0f * r, 2.0f * r);
    const float dashes[] = {theme::stroke::unplacedDash[0], theme::stroke::unplacedDash[1]};
    juce::PathStrokeType(theme::stroke::rule).createDashedStroke(ring, ring, dashes, 2);
    g.strokePath(ring, juce::PathStrokeType(theme::stroke::rule));
}

void ProbeView::paint(juce::Graphics& g) {
    clearRegions();
    const auto vp = viewport();
    //  asked before the energy is drawn: an open region's wash sits under the energy and is invisible
    //  against an undimmed peak, so the energy has to know one is open. Drawn after it, in `paintRegions`
    openRegion_ = gatherRegions(region_, regions_, state_.regionsAlways);

    paintFrame(g, vp);
    paintEnergy(g, vp);
    paintGraticule(g, vp);
    paintAxisLabels(g, vp);
    paintRegions(g, vp);
    paintAnswer(g, vp);
    paintProbe(g, vp);

    //  a click places it; a drag orbits the globe and never places. The split is `HitArea`'s, which
    //  applies the slop and does not call `click` after a drag -- there is no second statement of it
    //  here, and `drag` is only ever called once the slop is past
    HitArea::Region all;
    all.area = getLocalBounds().toFloat();
    all.press = [this](juce::Point<float> p) {
        pressedAt_ = p;
        //  a handle takes the press before the orbit does, and then it is not a click on the sphere
        pressOnHandle_ = grip_.press(projection_, state_.camera, viewport(), p);
    };
    all.drag = [this](juce::Point<float> p, bool) {
        //  a held handle is re-applied every frame, not only on pointer moves -- a region turning
        //  under a rate otherwise slips from under a still pointer, 103 px on the globe at 30 deg/s.
        //  Carrying it to where the pointer is, every drag, is that rule
        if (grip_.held()) {
            grip_.moveTo(p);
            carryHeld();
            return;
        }
        if (projection_ != Projection::Globe) return;  // the equirect does not orbit: it IS the whole sphere
        orbitView(state_, p.x - pressedAt_.x, p.y - pressedAt_.y);
        pressedAt_ = p;
        if (changed_) changed_();
    };
    all.release = [this] { grip_.release(); };
    all.doubleClick = [this] {
        //  a handle back to its default, as a double-click on its tile does
        if (grip_.reset(region_, projection_, state_.camera, viewport(), pressedAt_) && changed_) changed_();
    };
    all.click = [this] {
        //  a press on a handle is never a click on the sphere: grabbing a handle and letting go
        //  without moving would place the probe under it, jumping the ping away from where it was
        if (pressOnHandle_) return;
        const auto vpNow = viewport();
        placeAt((pressedAt_.x - vpNow.cx) / vpNow.rx, (vpNow.cy - pressedAt_.y) / vpNow.ry);
    };
    addRegion(std::move(all));

    //  last, so the strip's controls are registered after the whole-area region above and take the
    //  press from it: `HitArea` puts later regions on top. Registered first, a press on a toggle
    //  would orbit
    addSceneStripClicks(*this, strip_, state_, relayout_, changed_);
}

int ProbeView::handleAt(juce::Point<float> at) const { return grip_.at(projection_, state_.camera, viewport(), at); }

/// The held handle, carried to where the pointer is: on every move, and once a frame (`RegionGrip`).
void ProbeView::carryHeld() {
    if (grip_.carry(region_, projection_, state_.camera, viewport()) && changed_) changed_();
}

juce::Point<float> ProbeView::screenOf(Vec3 direction) const {
    const auto s = toScreen(projection_, state_.camera, viewport(), direction);
    return {static_cast<float>(s.x), static_cast<float>(s.y)};
}

void ProbeView::mouseMove(const juce::MouseEvent& e) {
    if (grip_.hover(projection_, state_.camera, viewport(), e.position)) repaint();
    setMouseCursor(grip_.hovering() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void ProbeView::mouseExit(const juce::MouseEvent&) {
    if (grip_.unhover()) repaint();
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ProbeView::followRegions() {
    gatherRegions(region_, regionsNow_, state_.regionsAlways);
    if (regionsNow_ == regions_) return;
    ++regionRepaintsAsked_;
    repaint();
}

}  // namespace bambi::ui
