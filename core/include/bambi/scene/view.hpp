// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <span>
#include <vector>

#include "bambi/math/vec3.hpp"
#include "bambi/path/authoring.hpp"
#include "bambi/path/trajectory.hpp"

namespace bambi {

/*  The scene's geometry, with no drawing in it: projections, hit-testing and interaction rules live
 *  here, headless and tested; the JUCE layer draws what this computes and forwards events to it.
 *  Conventions:
 *
 *    - the listener's left is screen left, in the globe as in the equirect
 *    - the globe is orthographic, and a point faces the viewer when its depth is positive
 *    - the equirect runs back, left, front, right, back from left to right, stretched to its frame
 *    - one model of the sphere behind every view: a projection is forward(p) and inverse(x, y)
 *      in normalised space, and everything else is written once against that
 */

enum class Projection { Globe, Equirect };

/// The globe's viewpoint. Pitch is positive looking down from above.
struct Camera {
    double yaw{0.55};  ///< three-quarter, from above
    double pitch{0.30};

    bool operator==(const Camera&) const = default;
};

/// The globe's presets. Since left stays on screen left, the top view has front at the bottom.
enum class ViewPreset { Free, Top, Front, Side };
Camera cameraFor(ViewPreset preset);

/// Dragging rotates the globe. Pitch stops at the poles.
inline constexpr double kOrbitRadPerPixel = 0.008;
void orbit(Camera& camera, double dxPixels, double dyPixels);

/// A point in normalised view space: x and y in [-1, 1], y up; depth > 0 faces the viewer.
struct ViewPoint {
    double x{0.0}, y{0.0}, depth{1.0};
};

ViewPoint project(Projection kind, const Camera& camera, Vec3 p);

/// The point on the sphere under a normalised position, if there is one there.
std::optional<Vec3> unproject(Projection kind, const Camera& camera, double nx, double ny);

/// For a drag: off the sphere, the nearest point on the visible rim (globe) or edge (equirect).
Vec3 unprojectToRim(Projection kind, const Camera& camera, double nx, double ny);

/*  Where normalised space sits in pixels: `cx, cy` the centre, `rx, ry` what a normalised 1 spans --
 *  equal for the globe, stretched to the frame for the equirect. Screen y grows downward. */
struct Viewport {
    double cx{0.0}, cy{0.0}, rx{1.0}, ry{1.0};
    double screenX(double nx) const { return cx + nx * rx; }
    double screenY(double ny) const { return cy - ny * ry; }

    bool operator==(const Viewport&) const = default;
};

struct ScreenPoint {
    double x{0.0}, y{0.0}, depth{1.0};
};
ScreenPoint toScreen(Projection kind, const Camera& camera, const Viewport& vp, Vec3 p);

/*  A polyline in screen segments. A segment is front when its mean depth is positive, so the back
 *  hemisphere can be drawn fainter; in the equirect everything is front. A segment crossing the
 *  equirect's back seam is split at the frame edge rather than drawn across the whole panel. */
struct ScreenSegment {
    double x0{0.0}, y0{0.0}, x1{0.0}, y1{0.0};
    bool front{true};
};
void projectPolyline(Projection kind, const Camera& camera, const Viewport& vp, std::span<const Vec3> points,
                     bool closed, std::vector<ScreenSegment>& out);

/// Latitude and meridian lines every 30 degrees, in world space; the equator and front meridian are major.
struct GraticuleLine {
    std::vector<Vec3> points;
    bool major{false};
};
const std::vector<GraticuleLine>& graticule();

/// Selecting an instance: its dot or its path. A dot wins over a path; on the globe only the facing
/// side can be hit.
inline constexpr double kDotHitPixels = 13.0;
inline constexpr double kPathHitPixels = 14.0;

struct SceneTarget {
    Vec3 position{1.0, 0.0, 0.0};  ///< where the source is now
    std::span<const Vec3> path;    ///< as drawn, transform applied; may be empty
    bool closed{true};
};

/// The index of the target under (x, y) in pixels, or -1.
int pick(Projection kind, const Camera& camera, const Viewport& vp, std::span<const SceneTarget> targets, double x,
         double y);

// Editing a custom chain in the scene; as with selection, only the facing side can be hit.
inline constexpr double kNodeHitPixels = 13.0;
inline constexpr double kHandleHitPixels = 12.0;
inline constexpr double kInsertHitPixels = 14.0;

/// An insertion lands strictly inside a segment: at either end it would land on a node already there.
inline constexpr double kInsertTMin = 0.03;
inline constexpr double kInsertTMax = 0.97;

/// The chain's nodes where the scene draws them: authored positions carried through the path's transform.
void transformedNodes(const TrajectoryState& s, Vec3 centre, const PathTransform& t, std::vector<Vec3>& out);

/// The node under (x, y) in pixels, or -1. `nodes` are drawn positions, as transformedNodes returns them.
int pickNode(Projection kind, const Camera& camera, const Viewport& vp, std::span<const Vec3> nodes, double x,
             double y);

/// The selected node's live handle under (x, y).
struct HandleHit {
    bool found{false};
    Handle which{Handle::In};
};
HandleHit pickHandle(Projection kind, const Camera& camera, const Viewport& vp, Vec3 in, bool inLive, Vec3 out,
                     bool outLive, double x, double y);

/*  Where a click falls on the curve, searched in screen space so a foreshortened view inserts where
 *  the user clicked. Callers clamp `t` to kInsertTMin/Max. The equirect's seam offers no insertion.
 */
struct CurveHit {
    bool found{false};
    std::size_t segment{0};
    double t{0.0};
    double distancePixels{0.0};
    Vec3 position{1.0, 0.0, 0.0};  ///< on the curve as drawn, for the insertion preview
};
CurveHit nearestOnCurveScreen(Projection kind, const Camera& camera, const Viewport& vp, const TrajectoryState& s,
                              Vec3 centre, const PathTransform& t, double x, double y);

/*  How far a press may move and still be a click. A click that eats the drag makes the globe
 *  undraggable, so the split is measured from the press and never from where the press landed.
 *
 *  The number lives here, in core; applying it is `ui::HitArea`'s.
 */
inline constexpr double kClickSlopPixels = 4.0;

/*  What a source's width covers, as a grid of screen cells.
 *
 *  Width is the cap's angular radius -- the alpha of capWeights, which the encoder is handed unhalved -- so 90
 *  is a hemisphere and 180 the whole sphere. A cap that big, or one reaching round the globe's edge, over a
 *  pole or across the equirect's back seam, is no ellipse on screen, and an outline filled as one is wrong
 *  exactly there: a source on the globe's edge would show no width at all. So each cell asks whether its
 *  point on the sphere is inside, softened over one cell's angle. `back` is the globe's far side, which a view
 *  draws fainter, as it does paths; the equirect has none. Only the cap's bounding box is sampled.
 */
struct CapCoverage {
    int x0{0}, y0{0};  ///< pixels: the grid's top-left corner
    int cols{0}, rows{0};
    int cell{1};                     ///< pixels per cell, each way
    std::vector<float> front, back;  ///< 0..1 per cell, row by row; `back` empty in the equirect
};
void capCoverage(Projection kind, const Camera& camera, const Viewport& vp, Vec3 centre, double radiusRad,
                 int cellPixels, CapCoverage& out);

}  // namespace bambi
