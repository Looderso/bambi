// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

#include "bambi/math/vec3.hpp"
#include "bambi/path/shape.hpp"

namespace bambi {

enum class MovementMode { Wrap, PingPong, Once };  ///< matches motion.mode choice order

/*  A path is baked into a table parameterised by arc length, and playback only calls eval(s) with s
 *  in [0, 1] of normalised arc length -- so speed along the path depends only on the input signal,
 *  never on how the curve was authored.
 */
/// Table size by path length: a point at least every kMaxLutStepRad, within kMinLutSize..kMaxLutSize.
inline constexpr int kMinLutSize = 1024;
inline constexpr int kMaxLutSize = 16384;
inline constexpr double kMaxLutStepRad = 0.6 * kDeg2Rad;
int lutSizeFor(double lengthRad);

struct PathSample;

/// Working memory for a build. Once reserveParametric() has run, parametric builds do not allocate,
/// so a modulated path can be rebuilt in a control step. A custom chain may still grow it.
struct PathScratch {
    std::vector<PathSample> samples;
    std::vector<double> cumulative;
    void reserveParametric();
};

class Trajectory {
public:
    /// Rebuild from state. Allocates; call on change, never per block.
    void build(const TrajectoryState& s);
    /// The same, in `scratch`; allocation-free for a parametric path when both are reserved.
    void build(const TrajectoryState& s, PathScratch& scratch);
    /// Room for the largest parametric table, so a build into it allocates nothing.
    void reserveParametric() { lut_.reserve(static_cast<std::size_t>(kMaxLutSize)); }

    /// Position at normalised arc length. Open paths clamp, closed paths wrap.
    Vec3 eval(double s) const;

    double lengthRad() const { return length_; }
    bool closed() const { return closed_; }
    bool empty() const { return lut_.size() < 2; }

    /// The dense table itself, for drawing and nearest-point search.
    const std::vector<Vec3>& points() const { return lut_; }

    /// Free-running phase to normalised arc length: wrap jumps back, ping-pong reverses, once holds.
    static double phaseToS(double phase, MovementMode mode, bool closed);

private:
    std::vector<Vec3> lut_;
    double length_{0.0};
    bool closed_{true};
};

/// A point of the dense polyline, with the custom-chain segment and Bezier t it came from (0 for a
/// parametric path), so an editor click maps to an exact subdivision point.
struct PathSample {
    Vec3 p;
    std::size_t segment{0};
    double t{0.0};
};

std::vector<PathSample> samplePath(const TrajectoryState& s);
/// The same into `out`, which it clears: no allocation while `out` has the room.
void samplePathInto(const TrajectoryState& s, std::vector<PathSample>& out);

/*  Placement of a path on the sphere, applied after eval(s). `extent` scales every point's angle
 *  from the path's centre: 1 is as authored, 0 a point, above 1 it opens out toward the antipode.
 *  Yaw and pitch rotate the path relative to where it was authored, so all zeros is the identity.
 */
struct PathTransform {
    double yawRad{0.0};
    double pitchRad{0.0};
    double rollRad{0.0};
    double extent{1.0};
};

/// The axis a path shrinks toward: the pole of the path's plane, with the mean of its points only
/// choosing the sign. (The mean alone is the origin for a great circle.)
Vec3 pathCentre(std::span<const Vec3> points);

/// Extent, then roll about the path centre, then elevation, then azimuth. Order matters.
Vec3 applyTransform(Vec3 p, Vec3 centre, const PathTransform& t);

/*  applyTransform undone, so a drag on the drawn path moves the authored node. Extent cannot be
 *  undone near 0 or where points were pinned at the antipode; below kEditableExtent an editor should
 *  refuse to move nodes.
 */
inline constexpr double kEditableExtent = 0.05;

/// As far as the control opens: at 2 a great circle has reached the antipode and closed up again.
inline constexpr double kMaxExtent = 2.0;
Vec3 unapplyTransform(Vec3 p, Vec3 centre, const PathTransform& t);

}  // namespace bambi
