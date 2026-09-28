// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/SphereView.h"

#include <algorithm>
#include <memory>

#include "bambi/math/sphere.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Strokes.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
//  The anchors on the globe's equator, lowercase like every label.
struct Anchor {
    double azDeg;
    const char* name;
};
constexpr std::array<Anchor, 4> kAnchors{{{0.0, "f"}, {90.0, "l"}, {180.0, "b"}, {270.0, "r"}}};
}  // namespace

SphereView::SphereView(SceneState& state, bambi::Projection projection) : state_(state), projection_(projection) {
    setOpaque(true);
}

bambi::Viewport SphereView::viewport() const { return bambi::ui::sceneViewport(projection_, getWidth(), getHeight()); }

void SphereView::notify() {
    if (state_.changed) state_.changed();
}

void SphereView::strokeSegments(juce::Graphics& g, juce::Colour colour, float width, bool dashed) const {
    //  how a projected line is stroked is every sphere's (bambi/ui/Strokes.h)
    bambi::ui::strokeSegments(g, segments_, skipped_, colour, width, dashed);
}

void SphereView::paint(juce::Graphics& g) {
    if (onPaint) onPaint();
    clearRegions();
    namespace colour = theme::colour;
    const bool globe = projection_ == bambi::Projection::Globe;
    const auto vp = viewport();
    const auto strip = static_cast<float>(theme::scene::labelStrip);

    g.fillAll(colour::panel);
    /*  The caption, the globe's view presets and the equirect's corner are every scene view's. */
    strip_ = paintSceneStrip(g, getWidth(), projection_, state_);
    energyToggle_ = strip_.energy;
    regionsToggle_ = strip_.regions;
    fullToggle_ = strip_.full;

    //  the sphere's outline, or the equirect's frame
    const auto frame = juce::Rectangle<float>(static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry),
                                              static_cast<float>(2.0 * vp.rx), static_cast<float>(2.0 * vp.ry));
    g.setColour(colour::sphere);
    if (globe)
        g.drawEllipse(frame, theme::stroke::rule);
    else
        g.drawRect(frame, theme::stroke::rule);

    /*  The energy picture, under everything that is drawn in ink: what leaves this encoder, which is
        all an encoder has -- so it is all blue, and the orange source sits on it. It shows what
        position and width have become at the order the track renders at. Off until asked for, and
        never for another instance. */
    energyDrawn_ = false;
    if (state_.energyOn && state_.energyAvailable)
        energyDrawn_ = energyLayer_.paint(g, projection_, state_.camera, vp, state_.energy, 1.0f);

    //  the graticule, in world space through the projection
    for (const auto& line : bambi::graticule()) {
        segments_.clear();
        bambi::projectPolyline(projection_, state_.camera, vp, line.points, false, segments_);
        strokeSegments(g, line.major ? colour::graticuleMain : colour::graticule, theme::stroke::graticule, false);
    }

    //  axis letters: above the equirect's frame, on the globe's equator where it faces the viewer
    if (globe) {
        for (const auto& anchor : kAnchors) {
            const auto s =
                bambi::toScreen(projection_, state_.camera, vp, bambi::fromAzEl(anchor.azDeg * bambi::kDeg2Rad, 0.0));
            if (s.depth <= 0.0) continue;
            text(g, anchor.name, smallFont(), colour::axisLabel,
                 juce::Rectangle<float>(0.0f, 0.0f, strip, strip)
                     .withCentre({static_cast<float>(s.x), static_cast<float>(s.y)}),
                 juce::Justification::centred);
        }
    } else {
        static constexpr std::array<const char*, 5> kLetters{"b", "l", "f", "r", "b"};
        const auto y = static_cast<float>(vp.cy - vp.ry) - theme::scene::axisLabelGap;
        for (std::size_t i = 0; i < kLetters.size(); ++i) {
            const auto x = static_cast<float>(vp.screenX(-1.0 + 0.5 * static_cast<double>(i)));
            text(g, kLetters[i], smallFont(), colour::axisLabel,
                 juce::Rectangle<float>(0.0f, 0.0f, strip, strip).withCentre({x, y}), juce::Justification::centred);
        }
    }

    /*  The region, over the graticule and under the paths and sources: it is the ground a source
        moves over, so the thing being read has to sit on top of it. Its handles while
        its page is open, as the effects' spheres have them. */
    const int open = bambi::ui::gatherRegions(region_, regions_, state_.regionsAlways);
    regionLayer_.paint(g, projection_, state_.camera, vp, regions_);
    grip_.paint(g, projection_, state_.camera, vp, regions_, open);

    //  other instances first, the selected one on top
    /*  A source is one dot, or what its stereo input makes of it. Filled is where sound is;
        a ring is a place that is not a point of sound in itself:
            sum        a dot.
            mid/side   the dot is the mid, and two rings either side are where the side is aimed.
            stereo     two dots, left and right, and a ring between them where the source is --
                       what a click selects, and what the region source reads. */
    const auto mark = [&](bambi::Vec3 where, juce::Colour c, float radius, bool filled) {
        const auto s = bambi::toScreen(projection_, state_.camera, vp, where);
        const bool hidden = globe && s.depth <= 0.0;
        g.setColour(hidden ? c.withMultipliedAlpha(theme::scene::backAlpha) : c);
        const auto box = juce::Rectangle<float>(2.0f * radius, 2.0f * radius)
                             .withCentre({static_cast<float>(s.x), static_cast<float>(s.y)});
        if (filled)
            g.fillEllipse(box);
        else
            g.drawEllipse(box, theme::stroke::rule);
    };
    const auto drawSource = [&](const SceneInstance& instance, juce::Colour c, float radius) {
        if (!instance.hasPosition) return;
        const auto small = radius * theme::shape::pairScale;
        if (instance.inputMode == 2) {
            mark(instance.left, c, radius, true);
            mark(instance.right, c, radius, true);
            mark(instance.position, c, small, false);
            return;
        }
        mark(instance.position, c, radius, true);
        if (instance.inputMode == 1) {
            mark(instance.left, c, small, false);
            mark(instance.right, c, small, false);
        }
    };

    const SceneInstance* selected = nullptr;
    for (const auto& instance : state_.instances) {
        if (instance.id == state_.selected) {
            selected = &instance;
            continue;
        }
        segments_.clear();
        bambi::projectPolyline(projection_, state_.camera, vp, instance.path, instance.closed, segments_);
        strokeSegments(g, colour::otherInstance, theme::stroke::otherPath, true);
        drawSource(instance, colour::otherInstance, theme::shape::otherSourceDot);
    }
    if (selected != nullptr) {
        //  its width, under its path and dot, and only for the selected instance
        if (selected->hasPosition && selected->widthRad > 0.0) {
            bambi::capCoverage(projection_, state_.camera, vp, selected->position, selected->widthRad,
                               theme::scene::widthCell, coverage_);
            if (coverage_.cols > 0 && coverage_.rows > 0) {
                if (widthImage_.getWidth() != coverage_.cols || widthImage_.getHeight() != coverage_.rows)
                    widthImage_ = juce::Image(juce::Image::ARGB, coverage_.cols, coverage_.rows, false);
                {
                    juce::Image::BitmapData pixels(widthImage_, juce::Image::BitmapData::writeOnly);
                    for (int row = 0; row < coverage_.rows; ++row)
                        for (int col = 0; col < coverage_.cols; ++col) {
                            const auto k = static_cast<std::size_t>(row) * static_cast<std::size_t>(coverage_.cols) +
                                           static_cast<std::size_t>(col);
                            float cover = coverage_.front[k];
                            if (!coverage_.back.empty())
                                cover = std::max(cover, coverage_.back[k] * theme::scene::backAlpha);
                            pixels.setPixelColour(col, row, colour::width.withAlpha(cover * theme::scene::widthAlpha));
                        }
                }
                g.setOpacity(1.0f);
                g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                g.drawImage(widthImage_,
                            juce::Rectangle<float>(static_cast<float>(coverage_.x0), static_cast<float>(coverage_.y0),
                                                   static_cast<float>(coverage_.cols * coverage_.cell),
                                                   static_cast<float>(coverage_.rows * coverage_.cell)));
            }
        }
        segments_.clear();
        bambi::projectPolyline(projection_, state_.camera, vp, selected->path, selected->closed, segments_);
        strokeSegments(g, colour::trajectory, theme::stroke::trajectory, false);
        if (!selected->livePath.empty()) {
            //  where the engine has the path now, beside the set one
            segments_.clear();
            bambi::projectPolyline(projection_, state_.camera, vp, selected->livePath, selected->closed, segments_);
            strokeSegments(g, colour::liveValue, theme::stroke::liveOutline, false);
        }
        drawSource(*selected, colour::source, theme::shape::sourceDot);
    }

    paintChain(g, vp);

    //  something the user has to see: a refusal, for a moment
    if (state_.notice.isNotEmpty() && state_.noticeUntilMs > juce::Time::getMillisecondCounterHiRes()) {
        const auto f = labelFont();
        text(g, state_.notice, f, colour::text,
             {theme::scene::noticeInset, static_cast<float>(getHeight()) - theme::scene::noticeInset - f.getHeight(),
              static_cast<float>(getWidth()), f.getHeight()});
    }

    //  a hovered source gets its name beside it: a moving dot is hard to identify otherwise
    if (const auto* hovered = state_.find(state_.hovered); hovered != nullptr && hovered->hasPosition) {
        const auto s = bambi::toScreen(projection_, state_.camera, vp, hovered->position);
        if (!globe || s.depth > 0.0) {
            const auto f = labelFont();
            const auto x = static_cast<float>(s.x) + theme::scene::hoverLabelGap;
            text(g, hovered->label, f, colour::text,
                 {x, static_cast<float>(s.y) - f.getHeight(), textWidth(f, hovered->label) + 1.0f,
                  2.0f * f.getHeight()});
        }
    }

    //  last: what was drawn is what can be clicked, at the coordinates it was drawn at
    buildRegions();
}

const SceneInstance* SphereView::editable() const {
    if (!state_.editing || state_.chain.nodes.size() < 2) return nullptr;
    return state_.find(state_.selected);
}

void SphereView::chainNodes(std::vector<bambi::Vec3>& out) const {
    const auto* instance = editable();
    if (instance == nullptr) {
        out.clear();
        return;
    }
    //  Through the same transform the drawn path carries, so a node sits ON the curve it shapes.
    bambi::transformedNodes(state_.chain, instance->centre, instance->transform, out);
}

void SphereView::paintChain(juce::Graphics& g, const bambi::Viewport& vp) {
    namespace colour = theme::colour;
    namespace scene = theme::scene;
    const auto* instance = editable();
    if (instance == nullptr) return;
    const bool globe = projection_ == bambi::Projection::Globe;
    const auto& chain = state_.chain;
    chainNodes(nodePoints_);

    const auto drawn = [&](bambi::Vec3 p) { return bambi::applyTransform(p, instance->centre, instance->transform); };
    const auto screen = [&](bambi::Vec3 p) { return bambi::toScreen(projection_, state_.camera, vp, p); };

    //  the selected node's handles: a stem along the sphere, then the grip
    const std::size_t selected = static_cast<std::size_t>(std::max(state_.selectedNode, 0));
    if (state_.selectedNode >= 0 && selected < chain.nodes.size()) {
        const auto& node = chain.nodes[selected];
        for (const auto which : {bambi::Handle::In, bambi::Handle::Out}) {
            if (!bambi::handleIsLive(chain, selected, which))
                continue;  // an open path's outer handles belong to derived endpoints
            const auto end = which == bambi::Handle::In ? node.cin : node.cout;
            std::vector<bambi::Vec3> stem;
            stem.reserve(static_cast<std::size_t>(scene::handleStem) + 1);
            for (int i = 0; i <= scene::handleStem; ++i)
                stem.push_back(drawn(bambi::slerp(node.p, end, static_cast<double>(i) / scene::handleStem)));
            segments_.clear();
            bambi::projectPolyline(projection_, state_.camera, vp, stem, false, segments_);
            strokeSegments(g, colour::handle, theme::stroke::otherPath, false);

            const auto s = screen(drawn(end));
            const bool hidden = globe && s.depth <= 0.0;
            const bool hot = state_.hoveredHandle && state_.hoveredWhich == which;
            const auto r = hot ? scene::handleRadius + 1.5f : scene::handleRadius;
            g.setColour(hidden ? colour::handle.withMultipliedAlpha(scene::backAlpha) : colour::handle);
            g.fillEllipse(juce::Rectangle<float>(2.0f * r, 2.0f * r)
                              .withCentre({static_cast<float>(s.x), static_cast<float>(s.y)}));
        }
    }

    //  the nodes: a disc where the curve is smooth, a square where it corners
    for (std::size_t i = 0; i < nodePoints_.size(); ++i) {
        const auto s = screen(nodePoints_[i]);
        const bool hidden = globe && s.depth <= 0.0;
        const bool isSelected = static_cast<int>(i) == state_.selectedNode;
        const bool isHovered = static_cast<int>(i) == state_.hoveredNode;
        const auto at = juce::Point<float>{static_cast<float>(s.x), static_cast<float>(s.y)};
        const auto ink = hidden ? colour::node.withMultipliedAlpha(scene::backAlpha) : colour::node;

        if (isSelected || isHovered) {
            g.setColour(isSelected ? colour::selectedNode : colour::selectedNode.withMultipliedAlpha(scene::backAlpha));
            g.drawEllipse(
                juce::Rectangle<float>(2.0f * scene::nodeHoverRing, 2.0f * scene::nodeHoverRing).withCentre(at),
                theme::stroke::rule);
        }
        //  an open path's ends are derived from their neighbours: ringed, because they behave differently
        if (!chain.closed && (i == 0 || i + 1 == nodePoints_.size())) {
            g.setColour(hidden ? colour::trajectory.withMultipliedAlpha(scene::backAlpha) : colour::trajectory);
            g.drawEllipse(juce::Rectangle<float>(2.0f * scene::nodeEndRing, 2.0f * scene::nodeEndRing).withCentre(at),
                          theme::stroke::rule);
        }

        const auto r = isSelected ? scene::nodeSelected : scene::nodeRadius;
        const auto box = juce::Rectangle<float>(2.0f * r, 2.0f * r).withCentre(at);
        g.setColour(colour::nodeFill);
        if (chain.nodes[i].smooth)
            g.fillEllipse(box);
        else
            g.fillRect(box);
        g.setColour(ink);
        if (chain.nodes[i].smooth)
            g.drawEllipse(box, theme::stroke::trajectory);
        else
            g.drawRect(box, theme::stroke::trajectory);
    }

    //  where a click would insert one
    if (state_.insert.found && drag_ == Drag::None && state_.hoveredNode < 0 && !state_.hoveredHandle) {
        const auto s = screen(state_.insert.position);
        if (!globe || s.depth > 0.0) {
            const auto at = juce::Point<float>{static_cast<float>(s.x), static_cast<float>(s.y)};
            g.setColour(colour::insertMark);
            g.drawEllipse(juce::Rectangle<float>(2.0f * scene::insertRadius, 2.0f * scene::insertRadius).withCentre(at),
                          theme::stroke::rule);
            g.drawLine(at.x - scene::insertCross, at.y, at.x + scene::insertCross, at.y, theme::stroke::rule);
            g.drawLine(at.x, at.y - scene::insertCross, at.x, at.y + scene::insertCross, theme::stroke::rule);
        }
    }
}

bool SphereView::beginNodeGesture(juce::Point<float> at, const juce::ModifierKeys& mods) {
    const auto* instance = editable();
    if (instance == nullptr) return false;
    const auto vp = viewport();
    const auto& chain = state_.chain;
    const double x = at.x, y = at.y;

    //  A handle first: it sits on the curve it bends, and it is the smaller target.
    const std::size_t selected = static_cast<std::size_t>(std::max(state_.selectedNode, 0));
    if (state_.selectedNode >= 0 && selected < chain.nodes.size()) {
        const auto& node = chain.nodes[selected];
        const auto in = bambi::applyTransform(node.cin, instance->centre, instance->transform);
        const auto out = bambi::applyTransform(node.cout, instance->centre, instance->transform);
        const auto hit = bambi::pickHandle(projection_, state_.camera, vp, in,
                                           bambi::handleIsLive(chain, selected, bambi::Handle::In), out,
                                           bambi::handleIsLive(chain, selected, bambi::Handle::Out), x, y);
        if (hit.found) {
            const int index = static_cast<int>(selected);
            if (mods.isAltDown() && state_.editNodes)  // break the symmetry before the drag bends it
                state_.editNodes("corner", {}, [index](bambi::TrajectoryState& s) {
                    bambi::makeCorner(s, static_cast<std::size_t>(index));
                });
            drag_ = Drag::Handle;
            dragIndex_ = index;
            dragWhich_ = hit.which;
            return true;
        }
    }

    chainNodes(nodePoints_);
    const int node = bambi::pickNode(projection_, state_.camera, vp, nodePoints_, x, y);
    if (node >= 0) {
        if (mods.isShiftDown()) {
            if (chain.nodes.size() <= 2)
                state_.say("a chain keeps at least two nodes");
            else if (state_.editNodes) {
                state_.editNodes("delete node", {}, [node](bambi::TrajectoryState& s) {
                    bambi::deleteNode(s, static_cast<std::size_t>(node));
                });
                state_.selectedNode = -1;
            }
            notify();
            return true;
        }
        state_.selectedNode = node;
        drag_ = Drag::Node;
        dragIndex_ = node;
        notify();
        return true;
    }

    const auto curve =
        bambi::nearestOnCurveScreen(projection_, state_.camera, vp, chain, instance->centre, instance->transform, x, y);
    if (curve.found && state_.editNodes) {
        //  The cap is a limit, not a malfunction: an insert that silently did nothing would read as a bug.
        if (chain.nodes.size() >= static_cast<std::size_t>(bambi::kMaxNodes)) {
            state_.say(juce::String(bambi::kMaxNodes) + " nodes is the limit");
            return true;
        }
        const auto segment = curve.segment;
        const auto t = bambi::clampd(curve.t, bambi::kInsertTMin, bambi::kInsertTMax);
        const auto inserted = std::make_shared<std::size_t>(0);
        state_.editNodes("insert node", {}, [segment, t, inserted](bambi::TrajectoryState& s) {
            *inserted = bambi::insertNode(s, segment, t);  // applied here and now: the index is the editor's
        });
        state_.selectedNode = static_cast<int>(*inserted);
        drag_ = Drag::Node;
        dragIndex_ = state_.selectedNode;
        state_.insert = {};
        notify();
        return true;
    }
    return false;
}

void SphereView::dragNode(juce::Point<float> at) {
    const auto* instance = editable();
    if (instance == nullptr || !state_.editNodes) {
        drag_ = Drag::None;
        return;
    }
    const auto vp = viewport();
    const auto point = bambi::unproject(projection_, state_.camera, (at.x - vp.cx) / vp.rx, -(at.y - vp.cy) / vp.ry);
    if (!point) return;  // off the sphere: the drag has nowhere to land, so it does nothing

    //  Back through the transform: a node is authored untransformed and edited where it is drawn.
    const auto target = bambi::unapplyTransform(*point, instance->centre, instance->transform);
    const auto index = static_cast<std::size_t>(dragIndex_);
    if (drag_ == Drag::Node) {
        state_.editNodes("move node", "node." + juce::String(dragIndex_),
                         [index, target](bambi::TrajectoryState& s) { bambi::moveNode(s, index, target); });
    } else {
        const auto which = dragWhich_;
        state_.editNodes(
            "bend handle", "handle." + juce::String(dragIndex_) + (which == bambi::Handle::In ? ".in" : ".out"),
            [index, which, target](bambi::TrajectoryState& s) { bambi::setHandle(s, index, which, target); });
    }
}

bool SphereView::hoverChain(juce::Point<float> at) {
    const auto* instance = editable();
    if (instance == nullptr) return false;
    const auto vp = viewport();
    const auto& chain = state_.chain;
    const double x = at.x, y = at.y;

    const int wasNode = state_.hoveredNode;
    const bool wasHandle = state_.hoveredHandle;
    const bool hadInsert = state_.insert.found;

    state_.hoveredHandle = false;
    const std::size_t selected = static_cast<std::size_t>(std::max(state_.selectedNode, 0));
    if (state_.selectedNode >= 0 && selected < chain.nodes.size()) {
        const auto& node = chain.nodes[selected];
        const auto hit = bambi::pickHandle(projection_, state_.camera, vp,
                                           bambi::applyTransform(node.cin, instance->centre, instance->transform),
                                           bambi::handleIsLive(chain, selected, bambi::Handle::In),
                                           bambi::applyTransform(node.cout, instance->centre, instance->transform),
                                           bambi::handleIsLive(chain, selected, bambi::Handle::Out), x, y);
        state_.hoveredHandle = hit.found;
        state_.hoveredWhich = hit.which;
    }
    chainNodes(nodePoints_);
    state_.hoveredNode = state_.hoveredHandle ? -1 : bambi::pickNode(projection_, state_.camera, vp, nodePoints_, x, y);
    state_.insert = state_.hoveredHandle || state_.hoveredNode >= 0
                        ? bambi::CurveHit{}
                        : bambi::nearestOnCurveScreen(projection_, state_.camera, vp, chain, instance->centre,
                                                      instance->transform, x, y);

    if (wasNode != state_.hoveredNode || wasHandle != state_.hoveredHandle || hadInsert != state_.insert.found ||
        state_.insert.found)
        notify();
    return true;
}

int SphereView::instanceAt(float x, float y) const {
    //  Instances with no position yet have no dot; their paths still select them.
    std::vector<bambi::SceneTarget> targets;
    std::vector<int> index;
    targets.reserve(state_.instances.size());
    for (std::size_t i = 0; i < state_.instances.size(); ++i) {
        const auto& instance = state_.instances[i];
        bambi::SceneTarget t;
        t.position = instance.hasPosition ? instance.position : bambi::Vec3{0.0, 0.0, 0.0};
        t.path = instance.path;
        t.closed = instance.closed;
        targets.push_back(t);
        index.push_back(static_cast<int>(i));
    }
    const int hit = bambi::pick(projection_, state_.camera, viewport(), targets, x, y);
    return hit >= 0 ? index[static_cast<std::size_t>(hit)] : -1;
}

/*  What the scene does with the mouse, as regions rather than as mouse handlers. `HitArea` owns the
    press/drag/click split and its 4 px slop, so the scene no longer keeps a second one of its own.

    Order of registration is the interaction: later regions lie on top. The whole view goes down
    first and the strip's controls after it, so a press on a chip or a toggle never orbits.
*/
juce::Point<float> SphereView::screenOf(bambi::Vec3 direction) const {
    const auto s = bambi::toScreen(projection_, state_.camera, viewport(), direction);
    return {static_cast<float>(s.x), static_cast<float>(s.y)};
}

void SphereView::buildRegions() {
    HitArea::Region all;
    all.area = getLocalBounds().toFloat();
    all.press = [this](juce::Point<float> p) {
        pressedAt_ = p;
        drag_ = Drag::None;
        dragIndex_ = -1;
        /*  a node, a handle, an insert or a shift-delete takes the press; then a region's handle; anything
            else leaves it to orbit and select. Nodes are live only on the trajectory tab and a region's
            handles only while its page is open, so the two never compete. */
        pressConsumed_ =
            beginNodeGesture(p, pressModifiers()) || grip_.press(projection_, state_.camera, viewport(), p);
    };
    all.drag = [this](juce::Point<float> p, bool) {
        if (drag_ != Drag::None) {
            dragNode(p);
            return;
        }
        if (grip_.held()) {
            grip_.moveTo(p);
            carryHeld();
            return;
        }
        if (pressConsumed_ || projection_ != bambi::Projection::Globe)
            return;  // the equirect does not orbit: it IS the whole sphere
        orbitView(state_, p.x - pressedAt_.x, p.y - pressedAt_.y);
        pressedAt_ = p;
        notify();
    };
    all.release = [this] {
        grip_.release();
        if (drag_ == Drag::None) return;
        drag_ = Drag::None;
        dragIndex_ = -1;
        if (state_.endNodeEdit) state_.endNodeEdit();  // one drag, one undo step
    };
    all.doubleClick = [this] {
        //  a handle back to its default, as a double-click on its tile does
        if (grip_.reset(region_, projection_, state_.camera, viewport(), pressedAt_)) notify();
    };
    all.click = [this] {
        //  a source is never dragged: a click selects an instance, a drag moves the view.
        if (pressConsumed_) return;  // the chain took this press; selecting as well would do two things at once
        const int hit = instanceAt(pressedAt_.x, pressedAt_.y);
        if (hit >= 0) state_.select(state_.instances[static_cast<std::size_t>(hit)].id);
    };
    addRegion(std::move(all));

    //  the strip's controls, last: they lie on top, so a press on one never orbits
    addSceneStripClicks(*this, strip_, state_, relayout_, [this] { notify(); });
}

void SphereView::carryHeld() {
    if (grip_.carry(region_, projection_, state_.camera, viewport())) notify();
}

void SphereView::mouseMove(const juce::MouseEvent& e) {
    //  over a corner toggle nothing below it is hovered: they lie on top, so they hide what they cover
    if (regionsToggle_.contains(e.position) || fullToggle_.contains(e.position)) return;
    if (hoverChain(e.position)) return;
    //  a region handle under the pointer is drawn as if held, with a grab cursor
    if (grip_.hover(projection_, state_.camera, viewport(), e.position)) repaint();
    setMouseCursor(grip_.hovering() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (grip_.hovering()) return;
    const int hit = instanceAt(e.position.x, e.position.y);
    const bambi::Uuid id = hit >= 0 ? state_.instances[static_cast<std::size_t>(hit)].id : bambi::Uuid{};
    if (!(id == state_.hovered)) {
        state_.hovered = id;
        notify();
    }
}

void SphereView::mouseExit(const juce::MouseEvent&) {
    if (grip_.unhover()) repaint();
    setMouseCursor(juce::MouseCursor::NormalCursor);
    if (state_.hoveredNode >= 0 || state_.hoveredHandle || state_.insert.found) {
        state_.hoveredNode = -1;
        state_.hoveredHandle = false;
        state_.insert = {};
        notify();
    }
    if (!state_.hovered.isNil()) {
        state_.hovered = {};
        notify();
    }
}

}  // namespace bambi::ui
