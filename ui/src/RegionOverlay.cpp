// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/RegionOverlay.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "bambi/math/sphere.hpp"
#include "bambi/ui/Draw.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace sc = theme::scene;

/// Pixels a sample of the region's field.
constexpr int kRegionCell = sc::regionCell;

/// A vector already in the region's own frame, back out to the world.
Vec3 ownToWorldVec(const Region& r, Vec3 own) {
    //  `toOwnFrame` is the way in; the way out is its inverse, which the frame's own axes give.
    const Vec3 x = toOwnFrame(r, {1.0, 0.0, 0.0});
    const Vec3 y = toOwnFrame(r, {0.0, 1.0, 0.0});
    const Vec3 z = toOwnFrame(r, {0.0, 0.0, 1.0});
    //  rows of the forward matrix are the frame's axes, so its transpose carries back
    return unit({x.x * own.x + x.y * own.y + x.z * own.z, y.x * own.x + y.y * own.y + y.z * own.z,
                 z.x * own.x + z.y * own.y + z.z * own.z});
}

/// A direction in the region's own frame, back out to the world.
Vec3 ownToWorld(const Region& r, double colatitude, double azimuthRad) {
    const double s = std::sin(colatitude);
    return ownToWorldVec(r, {s * std::cos(azimuthRad), s * std::sin(azimuthRad), std::cos(colatitude)});
}

/// `v` turned by `angle` about `axis`, which must be a unit vector perpendicular to nothing.
Vec3 aboutAxis(Vec3 v, Vec3 axis, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return unit(v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c)));
}
}  // namespace

/*  The reference dot of a set: the one nearest the axis that is not on it, ties broken by x then y
    so the choice is the same every time and a handle does not jump between two equals. Empty for a
    pair, whose two dots are both on the axis. */
std::optional<Vec3> referenceDot(int dots) {
    std::optional<Vec3> best;
    double bestColatitude = kPi;
    for (const auto& v : dotDirections(dots)) {
        const double t = std::acos(std::clamp(v.z, -1.0, 1.0));
        if (t < 1e-6 || t > kPi - 1e-6) continue;  // on the axis: it can show neither a size nor a turn
        const bool better =
            t < bestColatitude - 1e-9 || (std::abs(t - bestColatitude) < 1e-9 && best.has_value() &&
                                          (v.x > best->x + 1e-9 || (std::abs(v.x - best->x) < 1e-9 && v.y > best->y)));
        if (better || !best.has_value()) {
            best = v;
            bestColatitude = t;
        }
    }
    return best;
}

void regionHandles(const Region& r, std::vector<RegionHandle>& into) {
    into.clear();
    if (r.kind == RegionKind::Everywhere) return;

    switch (r.kind) {
        case RegionKind::Spot: into.push_back({RegionHandle::Kind::Edge, ownToWorld(r, r.size, 0.0)}); break;
        case RegionKind::Band: {
            const double mid = kPi / 2.0 - r.bandElevation;
            into.push_back({RegionHandle::Kind::Elevation, ownToWorld(r, std::clamp(mid, 0.001, kPi - 0.001), 0.0)});
            into.push_back(
                {RegionHandle::Kind::Edge, ownToWorld(r, std::clamp(mid - r.thickness / 2.0, 0.001, kPi - 0.001),
                                                      sc::bandThickAzDeg * kDeg2Rad)});
            break;
        }
        case RegionKind::Sectors:
            into.push_back({RegionHandle::Kind::Roll, ownToWorld(r, sc::rollStemDeg * kDeg2Rad, 0.0)});
            into.push_back({RegionHandle::Kind::Edge, ownToWorld(r, kPi / 2.0, r.fill * kPi / std::max(1, r.sectors))});
            break;
        case RegionKind::Dots: {
            /*  On the dot, not on a generic stem: the reference anchors both handles on the dot
                nearest the axis, so the size handle sits on the edge of the dot it sizes and the
                roll handle on the dot it turns. A pair has its dots on the axis and can show
                neither a turn nor a second anchor, so it keeps the plain edge handle. */
            const auto ref = referenceDot(r.dots);
            if (!ref.has_value()) {
                into.push_back({RegionHandle::Kind::Edge, ownToWorld(r, r.dotSize, 0.0)});
                break;
            }
            into.push_back({RegionHandle::Kind::Roll, ownToWorldVec(r, *ref)});
            const auto k = unit(cross(Vec3{0.0, 0.0, 1.0}, *ref));
            into.push_back({RegionHandle::Kind::Edge, ownToWorldVec(r, aboutAxis(*ref, k, r.dotSize))});
            break;
        }
        default: break;
    }
    for (auto& h : into) h.onMap = h.direction;
    //  last, so a handle sitting on it is taken first. On the map at a pole, where the roll stem
    //  arrives when there is one: the glyph goes where the eye already is.
    const auto roll = std::find_if(into.begin(), into.end(),
                                   [](const RegionHandle& h) { return h.kind == RegionHandle::Kind::Roll; });
    auto onMap = aimShownIn(r, Projection::Equirect);
    if (aimOnMap(r).polar && roll != into.end()) onMap = fromAzEl(azimuth(roll->direction), elevation(onMap));
    into.push_back({RegionHandle::Kind::Aim, aimOf(r), onMap});
}

Vec3 aimShownIn(const Region& r, Projection projection) {
    if (projection == Projection::Globe) return aimOf(r);
    //  just off the pole, so the map keeps the azimuth the angles give
    constexpr double kOffPole = 89.99 * kDeg2Rad;
    const auto m = aimOnMap(r);
    return fromAzEl(m.azimuth, std::clamp(m.elevation, -kOffPole, kOffPole));
}

/*  The wash and the contour, sampled through `valueAt` on a grid in screen space -- which is what
    makes one routine draw every kind, and a kind the plugin gains later, without knowing any of
    them. A cell is `regionCell` pixels: the field is smooth, so its contour is found between
    samples and its wash interpolated between them. */
void RegionLayer::build(Built& into, const SceneRegion& scene) {
    ++builds_;
    into.valid = true;
    into.scene = scene;
    into.edge.clear();

    const auto left = grid_.left, top = grid_.top, wide = grid_.wide, tall = grid_.tall;
    const auto cell = static_cast<float>(kRegionCell);

    //  the wash: the side that passes, at its value, under the energy. Only the open slot's.
    into.washArea = {static_cast<float>(left), static_cast<float>(top), static_cast<float>(wide) * cell,
                     static_cast<float>(tall) * cell};
    if (!scene.open)
        into.wash = {};
    else if (into.wash.getWidth() != wide || into.wash.getHeight() != tall)
        into.wash = juce::Image(juce::Image::ARGB, wide, tall, true);
    else
        into.wash.clear(into.wash.getBounds());
    std::optional<juce::Image::BitmapData> pixels;
    if (scene.open) pixels.emplace(into.wash, juce::Image::BitmapData::readWrite);

    //  marching squares over a screen-space grid, with the crossing interpolated along each cell
    //  edge, so the contour draws as a smooth line rather than a chain of dots
    //  the globe shows a disc of the sphere; the equirect's frame is all of it, and a rectangle clips that
    const bool disc = grid_.projection == Projection::Globe;
    const auto onSphere = [&](juce::Point<float> p) {
        if (!disc) return true;
        const double u = (p.x - grid_.vp.cx) / grid_.vp.rx, v = (p.y - grid_.vp.cy) / grid_.vp.ry;
        return u * u + v * v <= 1.0;
    };
    const RegionField field(scene.region);
    above_.resize(static_cast<std::size_t>(wide + 1));
    here_.resize(static_cast<std::size_t>(wide + 1));
    const auto sampleRow = [&](int row, std::vector<float>& out) {
        const auto* dirs = grid_.directions.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(wide + 1);
        for (int i = 0; i <= wide; ++i) out[static_cast<std::size_t>(i)] = static_cast<float>(field.at(dirs[i]));
    };

    sampleRow(0, above_);
    juce::Path contour;
    float dashPhase = 0.0f;  ///< where another instance's dotted edge is in its pattern
    for (int row = 1; row <= tall; ++row) {
        sampleRow(row, here_);
        const auto y = static_cast<float>(top + row * kRegionCell);
        for (int i = 1; i <= wide; ++i) {
            const auto x = static_cast<float>(left + i * kRegionCell);
            //  the cell's four corners: (x-cell, y-cell) clockwise
            const float tl = above_[static_cast<std::size_t>(i - 1)], tr = above_[static_cast<std::size_t>(i)];
            const float bl = here_[static_cast<std::size_t>(i - 1)], br = here_[static_cast<std::size_t>(i)];

            if (pixels.has_value() && (tl > 0.0f || tr > 0.0f || bl > 0.0f || br > 0.0f) &&
                onSphere({x - 0.5f * cell, y - 0.5f * cell})) {
                const auto v = std::max({tl, tr, bl, br});
                //  straight into the row: the image's own format, already multiplied by its alpha
                auto* px = reinterpret_cast<juce::PixelARGB*>(pixels->getPixelPointer(i - 1, row - 1));
                px->set(colour::regionWash.withAlpha(sc::regionWashAlpha * v).getPixelARGB());
            }

            const auto in = [](float v) { return v >= 0.5f; };
            const int code = (in(tl) ? 8 : 0) | (in(tr) ? 4 : 0) | (in(br) ? 2 : 0) | (in(bl) ? 1 : 0);
            if (code == 0 || code == 15) continue;

            //  where 0.5 sits along each edge, linearly between its two corners
            const auto at = [](float a, float b) {
                const auto span = b - a;
                return std::abs(span) < 1e-9f ? 0.5f : std::clamp((0.5f - a) / span, 0.0f, 1.0f);
            };
            const juce::Point<float> north{x - cell + cell * at(tl, tr), y - cell};
            const juce::Point<float> east{x, y - cell + cell * at(tr, br)};
            const juce::Point<float> south{x - cell + cell * at(bl, br), y};
            const juce::Point<float> west{x - cell, y - cell + cell * at(tl, bl)};
            const auto segment = [&](juce::Point<float> from, juce::Point<float> to) {
                const bool a = onSphere(from), b = onSphere(to);
                if (!a && !b) return;  // a cell is a few pixels: a segment with both ends off it is off it
                if (a != b) {
                    //  the end that left is brought back to the limb, by halving
                    auto in = a ? from : to, out = a ? to : from;
                    for (int step = 0; step < 10; ++step) {
                        const auto mid = (in + out) * 0.5f;
                        (onSphere(mid) ? in : out) = mid;
                    }
                    (a ? to : from) = in;
                }
                if (scene.own) {
                    contour.startNewSubPath(from);
                    contour.lineTo(to);
                    return;
                }
                /*  Another instance's edge is dotted, a segment at a time, the pattern carried from one to
                    the next. The contour is thousands of separate cell-sized segments, emitted row by row, so
                    a dashed stroke of the whole would join each row's segments across the region's inside. */
                const float on = theme::stroke::foreignRegionDash[0];
                const float period = on + theme::stroke::foreignRegionDash[1];
                const float length = from.getDistanceFrom(to);
                for (float done = 0.0f; done < length;) {
                    const float at = std::fmod(dashPhase, period);
                    const float run = std::min((at < on ? on : period) - at, length - done);
                    if (at < on) {
                        contour.startNewSubPath(from + (to - from) * (done / length));
                        contour.lineTo(from + (to - from) * ((done + run) / length));
                    }
                    done += run;
                    dashPhase += run;
                }
            };
            switch (code) {
                case 1:
                case 14: segment(west, south); break;
                case 2:
                case 13: segment(south, east); break;
                case 3:
                case 12: segment(west, east); break;
                case 4:
                case 11: segment(north, east); break;
                case 6:
                case 9: segment(north, south); break;
                case 7:
                case 8: segment(west, north); break;
                //  saddles: both crossings, which is the honest reading of four corners
                case 5:
                    segment(west, north);
                    segment(south, east);
                    break;
                case 10:
                    segment(north, east);
                    segment(west, south);
                    break;
                default: break;
            }
        }
        above_.swap(here_);
    }

    into.edge = std::move(contour);
}

/*  Where each sample looks. A sample's direction depends on the view and not on any region, so it
    is worked out when the camera or the viewport moves and read by every region until they do. */
void RegionLayer::lookThrough(Projection projection, const Camera& camera, const Viewport& vp) {
    //  the equirect is the whole sphere: orbiting the globe beside it moves nothing in it
    if (grid_.valid && grid_.projection == projection && grid_.vp == vp &&
        (projection == Projection::Equirect || grid_.camera == camera))
        return;
    grid_.valid = true;
    grid_.projection = projection;
    grid_.camera = camera;
    grid_.vp = vp;
    grid_.left = static_cast<int>(std::floor(vp.cx - vp.rx));
    grid_.top = static_cast<int>(std::floor(vp.cy - vp.ry));
    grid_.wide = std::max(1, static_cast<int>(std::ceil(2.0 * vp.rx)) / kRegionCell);
    grid_.tall = std::max(1, static_cast<int>(std::ceil(2.0 * vp.ry)) / kRegionCell);
    const auto count = static_cast<std::size_t>(grid_.wide + 1) * static_cast<std::size_t>(grid_.tall + 1);
    grid_.directions.assign(count, Vec3{});
    /*  A node off the sphere looks at the nearest point on its edge -- the globe's limb, the
        equirect's frame -- and what is built is cut back to the sphere, so an edge runs on past the
        limb and is cut exactly there rather than stopping a cell short with a gap. The cut is made
        once, when the region is built: a disc as a graphics clip costs more every paint than the
        whole layer does. */
    constexpr double kInside = 1.0 - 1e-9;
    std::size_t at = 0;
    for (int row = 0; row <= grid_.tall; ++row)
        for (int i = 0; i <= grid_.wide; ++i, ++at) {
            double nx = (grid_.left + static_cast<double>(i) * kRegionCell - vp.cx) / vp.rx;
            double ny = (vp.cy - (grid_.top + static_cast<double>(row) * kRegionCell)) / vp.ry;
            if (projection == Projection::Equirect) {
                /*  Half a pixel inside the frame, not on it: the frame's top and bottom are the
                    poles, where every sector meets and the value is 0.5 exactly, and a row of
                    samples there closes each sector's edge along the frame. */
                const double inX = 1.0 - 0.5 / vp.rx, inY = 1.0 - 0.5 / vp.ry;
                nx = std::clamp(nx, -inX, inX);
                ny = std::clamp(ny, -inY, inY);
            } else if (const double r = std::hypot(nx, ny); r > kInside) {
                nx *= kInside / r;
                ny *= kInside / r;
            }
            if (const auto d = unproject(projection, camera, nx, ny); d.has_value()) grid_.directions[at] = unit(*d);
        }
    grid_.frame = juce::Rectangle<float>{static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry),
                                         static_cast<float>(2.0 * vp.rx), static_cast<float>(2.0 * vp.ry)}
                      .getSmallestIntegerContainer();
    for (auto& b : built_) b.valid = false;  // every region was sampled through the view that has just moved
}

void RegionLayer::paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                        const std::vector<SceneRegion>& regions) {
    if (regions.empty()) return;
    lookThrough(projection, camera, vp);
    built_.resize(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& scene = regions[i];
        auto& b = built_[i];
        if (!b.valid || !(b.scene == scene)) build(b, scene);

        {
            //  the grid overhangs the view by up to a cell; the disc was cut when the region was built
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(grid_.frame);
            if (b.wash.isValid()) {
                //  smoothed up to size: the value is a smooth field sampled every few pixels, so
                //  interpolating between samples is closer to it than a flat square a sample is
                g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
                g.setOpacity(1.0f);  // an image is drawn at the context's alpha: not whatever was painted last
                g.drawImage(b.wash, b.washArea);
            }
            if (!b.edge.isEmpty()) {
                g.setColour(scene.live ? colour::liveValue : scene.own ? colour::regionEdge : colour::otherInstance);
                g.strokePath(b.edge,
                             juce::PathStrokeType(scene.live ? theme::stroke::liveOutline : theme::stroke::regionEdge));
            }
        }
        if (b.edge.isEmpty()) continue;

        /*  The role label, on a slot whose tab is closed: with two regions drawn and neither open,
            an unlabelled pair of contours says which shapes exist and not which is which. The open
            one is labelled by the tab that is open, so it does not need one here. Another
            instance's is edge only -- its label would come on hover, and nothing hovers a region yet. */
        if (scene.open || !scene.own || scene.live || scene.role.isEmpty()) continue;
        const auto aim = aimShownIn(scene.region, projection);
        const auto at = project(projection, camera, aim);
        if (projection == Projection::Globe && at.depth <= 0.0)
            continue;  // its aim is round the back: you label what you can see
        g.setColour(colour::regionLabel);
        g.setFont(smallFont());
        const auto x = static_cast<float>(vp.screenX(at.x));
        const auto y = static_cast<float>(vp.screenY(at.y));
        g.drawText(scene.role,
                   juce::Rectangle<float>{x + sc::regionLabelGap, y - sc::regionLabelGap, sc::regionLabelWide,
                                          sc::regionLabelHigh},
                   juce::Justification::centredLeft, false);
    }
}

void paintRegionHandles(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                        const std::vector<RegionHandle>& handles, int held, int hovered) {
    //  the stem from the aim to the roll handle, along the sphere: without it a handle standing 45
    //  degrees off the aim reads as an unattached dot with nothing to say what it turns
    const auto roll = std::find_if(handles.begin(), handles.end(),
                                   [](const RegionHandle& h) { return h.kind == RegionHandle::Kind::Roll; });
    const auto aim = std::find_if(handles.begin(), handles.end(),
                                  [](const RegionHandle& h) { return h.kind == RegionHandle::Kind::Aim; });
    if (roll != handles.end() && aim != handles.end()) {
        std::vector<Vec3> stem;
        stem.reserve(static_cast<std::size_t>(sc::handleStem) + 1);
        for (int i = 0; i <= sc::handleStem; ++i)
            stem.push_back(slerp(aim->direction, roll->direction, static_cast<double>(i) / sc::handleStem));
        stem.front() = aim->shownIn(projection);  // at a pole, from where the aim is drawn
        std::vector<ScreenSegment> segments;
        projectPolyline(projection, camera, vp, stem, false, segments);
        for (const auto& seg : segments) {
            g.setColour(colour::handle.withAlpha(seg.front ? 1.0f : sc::backAlpha));
            g.drawLine(static_cast<float>(seg.x0), static_cast<float>(seg.y0), static_cast<float>(seg.x1),
                       static_cast<float>(seg.y1), theme::stroke::otherPath);
        }
    }

    for (std::size_t i = 0; i < handles.size(); ++i) {
        const auto& h = handles[i];
        const auto at = toScreen(projection, camera, vp, h.shownIn(projection));
        const float dim = at.depth <= 0.0 ? sc::backAlpha : 1.0f;
        const bool hot = static_cast<int>(i) == held || static_cast<int>(i) == hovered;
        const bool aim = h.kind == RegionHandle::Kind::Aim;

        //  the aim is a node -- white with a thin black outline -- and every other handle one filled
        //  colour, so what moves the region reads apart from what shapes it. Hovered or held, larger.
        const auto radius = (aim ? sc::nodeRadius : sc::handleRadius) + (hot ? sc::handleHotGrow : 0.0f);
        const juce::Rectangle<float> dot{static_cast<float>(at.x) - radius, static_cast<float>(at.y) - radius,
                                         2.0f * radius, 2.0f * radius};
        g.setColour((aim ? colour::nodeFill : colour::handle).withAlpha(dim));
        g.fillEllipse(dot);
        if (aim) {
            g.setColour(colour::node.withAlpha(dim));
            g.drawEllipse(dot, theme::stroke::rule);
        }
    }
}

int regionHandleAt(Projection projection, const Camera& camera, const Viewport& vp,
                   const std::vector<RegionHandle>& handles, juce::Point<float> at) {
    //  forward, so the aim -- which `regionHandles` pushes last -- is tested last and a handle
    //  sitting on top of it is taken first; walking backwards would put the aim first instead
    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
        const auto& h = handles[static_cast<std::size_t>(i)];
        const auto s = toScreen(projection, camera, vp, h.shownIn(projection));
        if (projection == Projection::Globe && s.depth <= 0.0) continue;  // you hit what you see
        const auto radius = h.kind == RegionHandle::Kind::Aim ? sc::aimHit : sc::handleHit;
        if (at.getDistanceFrom({static_cast<float>(s.x), static_cast<float>(s.y)}) <= radius) return i;
    }
    return -1;
}

int gatherRegions(const RegionHook& hook, std::vector<SceneRegion>& into, bool always) {
    into.clear();
    if (!hook.regions) return -1;
    hook.regions(into);
    if (!always)
        into.erase(std::remove_if(into.begin(), into.end(), [](const SceneRegion& r) { return !r.open; }), into.end());
    for (std::size_t i = 0; i < into.size(); ++i)
        if (into[i].open) return static_cast<int>(i);
    return -1;
}

void RegionGrip::paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                       const std::vector<SceneRegion>& regions, int open) {
    handles_.clear();
    if (open < 0 || open >= static_cast<int>(regions.size())) return;
    regionHandles(regions[static_cast<std::size_t>(open)].region, handles_);
    paintRegionHandles(g, projection, camera, vp, handles_, held_, held() ? -1 : hovered_);
}

bool RegionGrip::hover(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> p) {
    const int now = at(projection, camera, vp, p);
    if (now == hovered_) return false;
    hovered_ = now;
    return true;
}

bool RegionGrip::unhover() {
    if (hovered_ < 0) return false;
    hovered_ = -1;
    return true;
}

int RegionGrip::at(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> p) const {
    return regionHandleAt(projection, camera, vp, handles_, p);
}

bool RegionGrip::press(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> p) {
    held_ = at(projection, camera, vp, p);
    pointer_ = p;
    grab_ = {};
    if (!held()) return false;
    const auto on = toScreen(projection, camera, vp, handles_[static_cast<std::size_t>(held_)].shownIn(projection));
    grab_ = juce::Point<float>{static_cast<float>(on.x), static_cast<float>(on.y)} - p;
    return true;
}

bool RegionGrip::carry(const RegionHook& hook, Projection projection, const Camera& camera, const Viewport& vp) {
    if (!held() || !hook.carried) return false;
    const auto want = pointer_ + grab_;  // where the handle goes, not where the cursor is
    //  off the sphere the drag carries on, along its rim
    const auto to = unprojectToRim(projection, camera, (want.x - vp.cx) / vp.rx, (vp.cy - want.y) / vp.ry);
    hook.carried(handles_[static_cast<std::size_t>(held_)].kind, to);
    return true;
}

bool RegionGrip::reset(const RegionHook& hook, Projection projection, const Camera& camera, const Viewport& vp,
                       juce::Point<float> p) const {
    const int which = at(projection, camera, vp, p);
    if (which < 0 || which >= static_cast<int>(handles_.size()) || !hook.reset) return false;
    hook.reset(handles_[static_cast<std::size_t>(which)].kind);
    return true;
}

void appendOtherRegions(const LinkScene& scene, Product kind, const Uuid& shown, std::vector<SceneRegion>& into) {
    for (const auto& entry : scene.entries()) {
        if (entry.instance == shown || entry.product != kind) continue;
        const int count = std::min<int>(static_cast<int>(entry.dyn.regionCount), kMaxRegions);
        for (int k = 0; k < count; ++k) {
            const auto region = entry.dyn.regions[k].region();
            if (region.kind != RegionKind::Everywhere)  // the whole sphere: an edge round all of it says nothing
                into.push_back({region, {}, false, false});
        }
    }
}

namespace {
/*  Near enough to be the same picture: half a degree in every angle, half a hundredth in every fraction.
    Exact equality would never hold -- the live one crossed the bus as floats, and the set one's turn is
    read a frame apart from the step that turned it. */
bool drawnAlike(const Region& a, const Region& b) {
    constexpr double angle = 0.5 * kDeg2Rad, fraction = 0.005;
    const auto near = [](double x, double y, double tol) { return std::abs(std::remainder(x - y, 2.0 * kPi)) <= tol; };
    const auto close = [](double x, double y, double tol) { return std::abs(x - y) <= tol; };
    return a.kind == b.kind && a.side == b.side && a.sectors == b.sectors && a.dots == b.dots && a.seed == b.seed &&
           near(a.yaw, b.yaw, angle) && near(a.pitch, b.pitch, angle) && near(a.roll, b.roll, angle) &&
           close(a.size, b.size, angle) && close(a.softness, b.softness, angle) &&
           close(a.bandElevation, b.bandElevation, angle) && close(a.thickness, b.thickness, angle) &&
           close(a.dotSize, b.dotSize, angle) && near(a.evolve, b.evolve, angle) && close(a.fill, b.fill, fraction) &&
           close(a.coverage, b.coverage, fraction) && close(a.contrast, b.contrast, fraction) &&
           close(a.detail, b.detail, fraction);
}
}  // namespace

void appendLiveRegion(const LinkScene& scene, const Uuid& shown, int slot, const Region& set,
                      std::vector<SceneRegion>& into) {
    const auto* entry = scene.find(shown);
    if (entry == nullptr || slot < 0 || slot >= std::min<int>(static_cast<int>(entry->dyn.regionCount), kMaxRegions))
        return;
    const auto live = entry->dyn.regions[slot].region();
    if (live.kind == RegionKind::Everywhere || drawnAlike(live, set)) return;
    SceneRegion r{live, {}, false, true};
    r.live = true;
    into.push_back(r);
}

}  // namespace bambi::ui
