// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "bambi/math/vec3.hpp"

/*  What a trajectory is, as saved: its kind, its generator, and its nodes.
 *
 *  These live here rather than in patch/state.hpp because path/ needs them and patch/ does not need
 *  path/ in return -- the patch contains a trajectory, so it includes this, and the dependency runs
 *  one way. They serialise as part of the patch, by name, like everything else there.
 */
namespace bambi {

inline constexpr int kMaxGenParams = 6;

/*  The most nodes a custom trajectory may have. They are Bezier nodes: a handful already makes
 *  a complex spline, and past two dozen a path is not more expressive, only harder to edit.
 *  It also bounds a trajectory's size on the link bus. */
inline constexpr int kMaxNodes = 24;

enum class TrajectoryKind { Parametric, Custom };
enum class GeneratorType { Orbit, Lissajous, Wave, Arc, Spiral };

/// A cubic Bezier anchor with its two handles, all on the unit sphere.
struct Node {
    Vec3 p{1, 0, 0};
    Vec3 cin{1, 0, 0};
    Vec3 cout{1, 0, 0};
    bool smooth{true};

    friend bool operator==(const Node&, const Node&) = default;
};

/// Segment `seg`'s control points: its node, that node's out-handle, the next node's in-handle, the next node.
inline void segmentControls(const std::vector<Node>& nodes, std::size_t seg, Vec3 (&out)[4]) {
    const std::size_t next = (seg + 1) % nodes.size();
    out[0] = nodes[seg].p;
    out[1] = nodes[seg].cout;
    out[2] = nodes[next].cin;
    out[3] = nodes[next].p;
}

/// The cubic Bezier through four control points, in 3-space (not projected to the sphere).
inline Vec3 cubicBezier(const Vec3 (&c)[4], double t) {
    const double u = 1.0 - t;
    return c[0] * (u * u * u) + c[1] * (3 * u * u * t) + c[2] * (3 * u * t * t) + c[3] * (t * t * t);
}

struct TrajectoryState {
    TrajectoryKind kind{TrajectoryKind::Parametric};
    bool closed{true};
    GeneratorType generator{GeneratorType::Orbit};  ///< the plainest motion, as a new encoder starts
    std::array<double, kMaxGenParams> genParams{};
    std::vector<Node> nodes;  ///< custom only

    friend bool operator==(const TrajectoryState&, const TrajectoryState&) = default;
};

}  // namespace bambi
