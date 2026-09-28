// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

#include "bambi/math/vec3.hpp"
#include "bambi/path/shape.hpp"

namespace bambi {

/*  Editing a custom Bezier chain, and converting a parametric trajectory into one. Every function
 *  mutates TrajectoryState in place; wrap calls in UndoStack::perform.
 */

enum class Handle { In, Out };

/// Handle length limits for interactive editing. Fitting uses a span-relative clamp instead.
inline constexpr double kHandleMinRad = 1.0 * kDeg2Rad;
inline constexpr double kHandleMaxRad = 80.0 * kDeg2Rad;

// ---- queries ---------------------------------------------------------------------------

/// Whether a handle shapes the curve. An open path's two outer handles do not.
bool handleIsLive(const TrajectoryState& s, std::size_t index, Handle which);

// ---- node operations -------------------------------------------------------------------

/*  Insert a node at (segment, t) by De Casteljau subdivision, so the curve's shape does not change.
 *  Returns the new node's index, or the node count if the segment is out of range or the chain is
 *  full.
 */
std::size_t insertNode(TrajectoryState& s, std::size_t segment, double t);

/// Remove a node. Refused when it would leave fewer than two.
bool deleteNode(TrajectoryState& s, std::size_t index);

/// Move a node, carrying its handles rigidly so the local shape survives the drag.
void moveNode(TrajectoryState& s, std::size_t index, Vec3 to);

/// Aim a handle at a target point; mirrors the opposite one when the node is smooth.
void setHandle(TrajectoryState& s, std::size_t index, Handle which, Vec3 target);

void makeSmooth(TrajectoryState& s, std::size_t index);
void makeCorner(TrajectoryState& s, std::size_t index);

/// Rotate the list so `index` becomes node 0 — chooses where the seam falls when opening.
void makeStart(TrajectoryState& s, std::size_t index);

/// Open cuts the closing segment; close restores it.
void setClosed(TrajectoryState& s, bool closed);

// ---- conversion ------------------------------------------------------------------------

struct FitResult {
    std::size_t nodes{0};
    double maxDeviationRad{0.0};
};

/// Parametric to custom, one way. Nodes go at curvature extrema and inflections first, then into
/// the widest remaining gaps, so peaks are kept.
FitResult convertToCustom(TrajectoryState& s, int targetNodes = 16);

/// Fit a node chain to a dense polyline. `targetNodes` is clamped to kMaxNodes.
FitResult fitNodes(TrajectoryState& s, const std::vector<Vec3>& dense, int targetNodes, bool closed);

}  // namespace bambi
