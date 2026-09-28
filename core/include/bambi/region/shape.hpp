// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>

#include "bambi/math/vec3.hpp"

/*  A region: where on the sphere something happens -- the geometric half, giving a shape's exact,
 *  order-free value in one direction. projection.hpp's gain matrix is this, band-limited: it
 *  truncates to the field's order, so its edges ring outside 0..1.
 *
 *  Radians everywhere in core; degrees at the parameter and UI boundary. */
namespace bambi {

/// Five shapes defined by an edge in space, plus two weights kinds (clouds, custom) whose weights
/// are all there is: their value is the sum of weights times harmonics, in the region's own frame.
enum class RegionKind { Everywhere, Spot, Band, Sectors, Dots, Clouds, Custom };

/// A weights kind carries this many, up to this order: the highest order the bus runs at.
inline constexpr int kRegionWeightsOrder = 7;
inline constexpr int kRegionWeights = (kRegionWeightsOrder + 1) * (kRegionWeightsOrder + 1);
inline constexpr int kMaxCloudSeed = 99;

enum class RegionSide { Inside, Outside };

/// A shape authored pointing up, plus an orientation: keeps its projected weights sparse (one
/// column, one diagonal) at no cost here.
struct Region {
    RegionKind kind{RegionKind::Everywhere};
    RegionSide side{RegionSide::Inside};

    /// Turned from its zero pose: roll about its own axis, then pitch about +y (front toward up),
    /// then yaw about +z (CCW from front). At zero a spot faces front; band, sectors and dots
    /// stand up (sector 0's centre at front); clouds and custom sit in the world frame.
    double yaw{0.0};
    double pitch{0.0};
    double roll{0.0};

    /// How wide the edge fades, in radians. Below kHardEdge the edge is a step.
    double softness{0.0};

    double size{kPi / 4};       ///< spot: half-angle
    double bandElevation{0.0};  ///< band: where its centre sits
    double thickness{kPi / 6};  ///< band: top to bottom
    /*  Sectors are singular on the aim axis, where every edge meets, and narrow in arc near it, so
     *  close enough the fade is wider than the sector and its centre never reaches 1. At fill 0 the
     *  sector is its own centre line; a hard edge reads that line as inside. */
    int sectors{4};           ///< sectors: how many
    double fill{0.5};         ///< sectors: the share of each period that is inside, 0..1
    int dots{6};              ///< dots: 2, 4, 6, 8, 12 or 20 -- corners of a regular solid
    double dotSize{kPi / 9};  ///< dots: half-angle of each

    /*  Clouds: seed picks the pattern; coverage is the field's average; contrast is twice its
     *  standard deviation; detail sets how fast weights fall off with order; evolve (radians) turns
     *  between two perpendicular patterns, looping at a full turn. Custom: the weights directly. */
    int seed{1};
    double coverage{0.5};
    double contrast{0.6};
    double detail{0.5};
    double evolve{0.0};
    std::array<double, kRegionWeights> weights{};

    /// Exact, field by field: lets a caller detect whether a region has moved.
    bool operator==(const Region&) const = default;
};

/*  Below this the edge is a step rather than a ramp. It is a guard against dividing by a softness
 *  of zero, not a musical value: half a degree is far finer than any order can draw. */
inline constexpr double kHardEdge = 0.5 * kDeg2Rad;

/*  The region's value at a direction: 1 inside, 0 outside, softness wide of fade across the edge,
 *  so the edge itself always reads 0.5. Outside is exactly 1 - inside in every direction. */
double valueAt(const Region& r, Vec3 direction);

/*  How the edge fades: value at a signed angular distance `d`, positive inside -- the fraction of
 *  a disc of radius R = softness/2 lying inside a straight edge:
 *
 *      (acos(t) - t * sqrt(1 - t^2)) / pi,     t = -d / R
 *
 *  Exact for a flat edge; error grows where the edge's own curvature rivals the blur. */
double edgeProfile(double d, double softness);

/*  Signed angular distance to the shape's edge, positive inside, with the direction already
 *  carried into the region's own frame. Used directly by a UI hit-test and a drawn outline. */
double edgeDistance(const Region& r, Vec3 inOwnFrame);

/*  The weights of a weights kind, in its own frame, for the channels of `order`. Custom copies its
 *  weights; clouds builds them from the seed and four settings -- coverage on the omni, and above it
 *
 *      k * gain_n(detail) * (cos(evolve) * A_n + sin(evolve) * B_n),      n = 1..order
 *
 *  with A_n, B_n unit-length-sqrt(2n+1) vectors made perpendicular per order, gain_n = n^-p,
 *  p = 3(1-detail), and k set so the field's standard deviation at `order` is half the contrast. */
void regionWeights(const Region& r, int order, std::span<double> out);

/// Carry a world direction into the region's own frame, where the shape points up.
Vec3 toOwnFrame(const Region& r, Vec3 direction);

/// `valueAt` for many directions of one region: the rotation into the region's own frame is
/// precomputed once as a matrix instead of worked out per call. `at` takes a unit-length direction.
class RegionField {
public:
    explicit RegionField(const Region& r);
    double at(Vec3 unitDirection) const;

private:
    Region r_;
    Vec3 x_, y_, z_;                                ///< where the world's axes land in the region's own frame
    std::array<double, kRegionWeights> weights_{};  ///< a weights kind's, built once
};

/// The world direction the shape's axis points in.
Vec3 aimOf(const Region& r);

/*  The aim as a flat map places it: elevation, and azimuth read off the angles rather than the
 *  direction, so it stays defined at a pole. `polar` says the aim is on one (within ~0.5 degree). */
struct AimOnMap {
    double azimuth{0.0}, elevation{0.0};
    bool polar{false};
};
AimOnMap aimOnMap(const Region& r);

/*  Points the shape's axis at a world direction, taking whichever of two yaw/pitch solutions is
 *  nearer the present orientation. Where a turn about the axis is visible (sectors, dots other
 *  than two), roll is rewritten to carry the pattern along the shortest arc rather than spin it. */
void aimAt(Region& r, Vec3 direction);

/*  A region as a plugin holds it: kind and counts are state; everything else is parameters, in
 *  degrees. `resolveRegion` is the one place these meet and degrees become radians. */
struct RegionShape {
    RegionKind kind{RegionKind::Spot};  ///< the shape; never Custom here -- that is `custom`, beside it
    int sectors{4};
    int dots{6};
    int seed{1};  ///< clouds': set once
    /// A flag beside the kind: the shape's own parameters stay underneath custom, so picking the
    /// shape again shows them as they were. `weights` are frozen from a shape, edited by hand.
    bool custom{false};
    std::array<double, kRegionWeights> weights{};
    /// A gain per order, beside the weights: `resolveRegion` plays weight times its order's gain.
    /// Clouds shape their orders with `detail` and ignore these.
    std::array<double, kRegionWeightsOrder + 1> gains{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

    friend bool operator==(const RegionShape&, const RegionShape&) = default;
};

/// A region's turn rates, degrees per second. Apart from `RegionSettingsDeg` because a setting is
/// where the region is and a rate is how it moves; `resolveRegion` takes the accumulated turn.
struct RegionRatesDeg {
    double yaw{0.0}, pitch{0.0}, roll{0.0};
};

struct RegionSettingsDeg {
    double yaw{0.0}, pitch{0.0}, roll{0.0};
    double softness{20.0};
    double size{45.0};
    double bandElevation{0.0};
    double thickness{30.0};
    double fill{0.5};
    double dotSize{20.0};
    double coverage{0.5};
    double contrast{0.6};
    double detail{0.5};
    double evolve{0.0};  ///< degrees
};

inline constexpr int kMaxSectors = 32;

/// A shape that can be evaluated: sectors within 1..kMaxSectors, and a dot count that is not one of
/// the six solids becomes 6.
RegionShape sanitised(RegionShape shape);

/*  `turnRad` is what a rate has turned each axis by since it was last reset, added to the set angle.
 *  Dot size is held to the largest at which neighbouring dots do not touch. A setting that is not a
 *  number takes its default. */
Region resolveRegion(const RegionShape& shape, RegionSide side, const RegionSettingsDeg& settings,
                     double yawTurnRad = 0.0, double pitchTurnRad = 0.0, double rollTurnRad = 0.0);

/*  The dot directions for a count: the corners of a regular solid, so they are evenly spread and
 *  the set is symmetric. Empty for a count that is not one of 2, 4, 6, 8, 12, 20. */
std::span<const Vec3> dotDirections(int count);

/*  The largest dotSize at which neighbouring dots still do not touch: half the smallest angle
 *  between two of them. Zero for an unsupported count. */
double maxDotSize(int count);

}  // namespace bambi
