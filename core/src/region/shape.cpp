// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/region/shape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "bambi/math/sh.hpp"
#include "bambi/math/sphere.hpp"

namespace bambi {
namespace {

constexpr Vec3 kUp{0.0, 0.0, 1.0};

/*  The corners of the regular solids, normalised. Computed once, when the library loads, rather
 *  than written out: the icosahedron and dodecahedron are golden-ratio triples, and a
 *  transcription error in one of sixty numbers is exactly the kind of thing that hides. */

struct DotSets {
    std::array<Vec3, 2> two{};
    std::array<Vec3, 4> four{};
    std::array<Vec3, 6> six{};
    std::array<Vec3, 8> eight{};
    std::array<Vec3, 12> twelve{};
    std::array<Vec3, 20> twenty{};

    DotSets() {
        const double kPhi = (1.0 + std::sqrt(5.0)) * 0.5;
        two = {kUp, Vec3{0.0, 0.0, -1.0}};
        four = {unit(Vec3{1, 1, 1}), unit(Vec3{1, -1, -1}), unit(Vec3{-1, 1, -1}), unit(Vec3{-1, -1, 1})};
        six = {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, kUp, Vec3{0, 0, -1}};

        std::size_t k = 0;
        for (const double x : {1.0, -1.0})
            for (const double y : {1.0, -1.0})
                for (const double z : {1.0, -1.0}) eight[k++] = unit(Vec3{x, y, z});

        k = 0;
        for (const double a : {1.0, -1.0})
            for (const double b : {1.0, -1.0}) {
                twelve[k++] = unit(Vec3{0.0, a, b * kPhi});
                twelve[k++] = unit(Vec3{a, b * kPhi, 0.0});
                twelve[k++] = unit(Vec3{b * kPhi, 0.0, a});
            }

        //  A dodecahedron's twenty corners are a cube's eight plus three golden rectangles.
        k = 0;
        for (const Vec3& v : eight) twenty[k++] = v;
        for (const double a : {1.0, -1.0})
            for (const double b : {1.0, -1.0}) {
                twenty[k++] = unit(Vec3{0.0, a / kPhi, b * kPhi});
                twenty[k++] = unit(Vec3{a / kPhi, b * kPhi, 0.0});
                twenty[k++] = unit(Vec3{b * kPhi, 0.0, a / kPhi});
            }
    }
};

/*  At namespace scope, not a function-local static: the first use of one of those takes a lock, and
 *  the first use of this can be on the audio thread. */
const DotSets kSets;

}  // namespace

std::span<const Vec3> dotDirections(int count) {
    const auto& s = kSets;
    switch (count) {
        case 2: return s.two;
        case 4: return s.four;
        case 6: return s.six;
        case 8: return s.eight;
        case 12: return s.twelve;
        case 20: return s.twenty;
        default: return {};
    }
}

double maxDotSize(int count) {
    const auto d = dotDirections(count);
    if (d.empty()) return 0.0;
    //  Half the smallest angle between two of them: any larger and a pair would overlap. Derived
    //  from the set rather than tabulated, so it cannot disagree with it.
    double smallest = kPi;
    for (std::size_t i = 0; i < d.size(); ++i)
        for (std::size_t j = i + 1; j < d.size(); ++j) smallest = std::min(smallest, arc(d[i], d[j]));
    return smallest * 0.5;
}

namespace {

static bool isWeightsKind(RegionKind k) { return k == RegionKind::Clouds || k == RegionKind::Custom; }

/*  Where a kind sits at zero. A spot faces front; a band lies on the horizon, sectors divide the
 *  space around the listener and dots stand on the vertical, so those three keep the up axis they
 *  were authored about, with their own x -- sector 0 -- at front. The axis is always in the x-z
 *  plane: pitch is about y, and could never lift an axis that pointed along it. */
bool sitsUpright(RegionKind k) {
    //  A weights kind sits in the world frame at zero: its weights are what a shape projected to
    //  at its own zero pose, so converting changes nothing that can be seen.
    return k == RegionKind::Band || k == RegionKind::Sectors || k == RegionKind::Dots || isWeightsKind(k);
}

/// Whether a turn about the shape's axis can be seen: not for a spot, a band, or two dots.
bool rollShows(const Region& r) { return r.kind == RegionKind::Sectors || (r.kind == RegionKind::Dots && r.dots != 2); }

Vec3 zeroAxis(const Region& r) { return sitsUpright(r.kind) ? Vec3{0, 0, 1} : Vec3{1, 0, 0}; }

Vec3 fromOwnFrame(const Region& r, Vec3 own) {
    //  Onto the zero pose -- for a front-facing kind a permutation of axes, so it is exact -- then
    //  the placement's three rotations: roll about the shape's axis, pitch, yaw.
    Vec3 v = sitsUpright(r.kind) ? own : Vec3{own.z, -own.y, own.x};
    if (r.roll != 0.0) v = rotateAxis(v, zeroAxis(r), r.roll);
    if (r.pitch != 0.0) v = rotateAxis(v, Vec3{0, 1, 0}, -r.pitch);
    if (r.yaw != 0.0) v = rotateAxis(v, Vec3{0, 0, 1}, r.yaw);
    return v;
}

double wrapPi(double a) { return std::remainder(a, 2.0 * kPi); }

}  // namespace

Vec3 toOwnFrame(const Region& r, Vec3 direction) {
    //  fromOwnFrame undone in the opposite order, as unapplyTransform does.
    Vec3 v = direction;
    if (r.yaw != 0.0) v = rotateAxis(v, Vec3{0, 0, 1}, -r.yaw);
    if (r.pitch != 0.0) v = rotateAxis(v, Vec3{0, 1, 0}, r.pitch);
    if (r.roll != 0.0) v = rotateAxis(v, zeroAxis(r), -r.roll);
    return sitsUpright(r.kind) ? v : Vec3{v.z, -v.y, v.x};
}

Vec3 aimOf(const Region& r) { return fromOwnFrame(r, {0, 0, 1}); }

AimOnMap aimOnMap(const Region& r) {
    //  pitch tilts the axis up from where the kind sits at zero; past a quarter turn it is over the top
    const double e = (sitsUpright(r.kind) ? kPi * 0.5 : 0.0) + r.pitch;
    const bool flipped = std::cos(e) < 0.0;
    return {wrapPi(r.yaw + (flipped ? kPi : 0.0)), std::asin(std::clamp(std::sin(e), -1.0, 1.0)),
            std::abs(std::cos(e)) < 0.009};
}

void aimAt(Region& r, Vec3 direction) {
    const Vec3 d = unit(direction);
    const Vec3 oldAim = aimOf(r);
    const Vec3 oldX = fromOwnFrame(r, {1, 0, 0});

    //  Two yaw and pitch pairs reach any direction, the second with pitch past a quarter turn. Take
    //  the one nearer where the orientation is, so a region pitched over the top is not folded back.
    const double alpha = sitsUpright(r.kind) ? kPi * 0.5 : 0.0;
    const bool polar = std::abs(d.x) < 1e-9 && std::abs(d.y) < 1e-9;
    const double az = polar ? r.yaw : azimuth(d), el = elevation(d);
    const double yawA = az, pitchA = el - alpha;
    const double yawB = az + kPi, pitchB = kPi - el - alpha;
    const auto far = [&](double yaw, double pitch) {
        return std::abs(wrapPi(yaw - r.yaw)) + std::abs(wrapPi(pitch - r.pitch));
    };
    const bool first = far(yawA, pitchA) <= far(yawB, pitchB);
    r.yaw = wrapPi(first ? yawA : yawB);
    r.pitch = wrapPi(first ? pitchA : pitchB);

    //  Yaw and pitch alone would turn a pattern about its axis on the way -- a quarter turn in the
    //  first half degree off a pole. Carry the old frame along the shortest arc instead, and read
    //  the roll that keeps the pattern where that put it.
    if (!rollShows(r) || dot(oldAim, d) < -1.0 + 1e-12) return;
    const Vec3 carriedX = rotateAToB(oldX, oldAim, d);
    Region bare = r;
    bare.roll = 0.0;
    r.roll = std::atan2(dot(carriedX, fromOwnFrame(bare, {0, 1, 0})), dot(carriedX, fromOwnFrame(bare, {1, 0, 0})));
}

namespace {
//  mulberry32 and Box-Muller: deterministic from the seed alone.
struct Mulberry {
    std::uint32_t a;
    double next() {
        a += 0x6D2B79F5u;
        std::uint32_t t = a;
        t = (t ^ (t >> 15)) * (t | 1u);
        t = (t + ((t ^ (t >> 7)) * (t | 61u))) ^ t;
        return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
    }
};
void gaussians(int seed, std::span<double> out) {
    Mulberry r{static_cast<std::uint32_t>(seed) * 9973u + 17u};
    for (std::size_t i = 0; i < out.size(); i += 2) {
        const double m = std::sqrt(-2.0 * std::log(std::max(r.next(), 1e-12))), v = 2.0 * kPi * r.next();
        out[i] = m * std::cos(v);
        if (i + 1 < out.size()) out[i + 1] = m * std::sin(v);
    }
}
}  // namespace

static void cloudBasis(int seed, int order, std::span<double> A, std::span<double> B) {
    const int N = std::clamp(order, 0, kRegionWeightsOrder);
    std::array<double, kRegionWeights> a{}, b{};
    gaussians(seed, a);
    gaussians(seed + 1000, b);
    a[0] = b[0] = 0.0;
    for (int n = 1; n <= N; ++n) {
        const int a0 = n * n, a1 = (n + 1) * (n + 1);
        const double want = std::sqrt(2.0 * n + 1.0);
        double aa = 0.0;
        for (int i = a0; i < a1; ++i) aa += a[static_cast<std::size_t>(i)] * a[static_cast<std::size_t>(i)];
        const double la = std::sqrt(aa) > 0.0 ? std::sqrt(aa) : 1.0;
        for (int i = a0; i < a1; ++i) a[static_cast<std::size_t>(i)] *= want / la;
        double ab = 0.0;
        for (int i = a0; i < a1; ++i) ab += a[static_cast<std::size_t>(i)] * b[static_cast<std::size_t>(i)];
        for (int i = a0; i < a1; ++i)
            b[static_cast<std::size_t>(i)] -= ab / (2.0 * n + 1.0) * a[static_cast<std::size_t>(i)];
        double bb = 0.0;
        for (int i = a0; i < a1; ++i) bb += b[static_cast<std::size_t>(i)] * b[static_cast<std::size_t>(i)];
        const double lb = std::sqrt(bb) > 0.0 ? std::sqrt(bb) : 1.0;
        for (int i = a0; i < a1; ++i) b[static_cast<std::size_t>(i)] *= want / lb;
    }
    const std::size_t C = static_cast<std::size_t>((N + 1) * (N + 1));
    for (std::size_t i = 0; i < A.size(); ++i) A[i] = i < C ? a[i] : 0.0;
    for (std::size_t i = 0; i < B.size(); ++i) B[i] = i < C ? b[i] : 0.0;
}

static double cloudOrderGain(int n, double detail) {
    return std::pow(static_cast<double>(std::max(n, 1)), -3.0 * (1.0 - clampd(detail, 0.0, 1.0)));
}

static double cloudScale(double contrast, double detail, int order) {
    double tot = 0.0;
    for (int n = 1; n <= std::min(order, kRegionWeightsOrder); ++n) {
        const double g = cloudOrderGain(n, detail);
        tot += g * g;
    }
    return tot > 0.0 ? (std::max(contrast, 0.0) * 0.5) / std::sqrt(tot) : 0.0;
}

void regionWeights(const Region& r, int order, std::span<double> out) {
    const int N = std::clamp(order, 0, kRegionWeightsOrder);
    const std::size_t C = static_cast<std::size_t>((N + 1) * (N + 1));
    std::fill(out.begin(), out.end(), 0.0);
    if (r.kind == RegionKind::Custom) {
        for (std::size_t i = 0; i < out.size() && i < C; ++i) out[i] = std::isfinite(r.weights[i]) ? r.weights[i] : 0.0;
        return;
    }
    if (r.kind != RegionKind::Clouds) return;
    std::array<double, kRegionWeights> A{}, B{};
    cloudBasis(r.seed, N, A, B);
    const double k = cloudScale(r.contrast, r.detail, N);
    const double c = std::cos(r.evolve), s = std::sin(r.evolve);
    if (!out.empty()) out[0] = clampd(r.coverage, 0.0, 1.0);
    for (std::size_t i = 1; i < out.size() && i < C; ++i) {
        const int n = static_cast<int>(std::sqrt(static_cast<double>(i)));
        out[i] = k * cloudOrderGain(n, r.detail) * (c * A[i] + s * B[i]);
    }
}

double edgeDistance(const Region& r, Vec3 own) {
    const double theta = std::acos(clampd(own.z, -1.0, 1.0));  // colatitude: 0 at the up pole
    switch (r.kind) {
        case RegionKind::Clouds:
        case RegionKind::Custom: return 0.0;  // no edge: the weights are all there is

        case RegionKind::Everywhere: return kPi;  // inside, everywhere, by any margin

        case RegionKind::Spot: return r.size - theta;

        case RegionKind::Band: return r.thickness * 0.5 - std::abs(theta - (kPi * 0.5 - r.bandElevation));

        case RegionKind::Sectors: {
            if (r.sectors < 1) return -kPi;
            const double period = 2.0 * kPi / static_cast<double>(r.sectors);
            const double phi = std::atan2(own.y, own.x);
            //  Fold azimuth into one period, centred on zero, so a sector straddles the axis.
            const double wrapped = std::fmod(std::fmod(phi + period * 0.5, period) + period, period);
            const double local = wrapped - period * 0.5;
            //  An azimuthal angle is a shorter arc near the poles, so the distance to a sector's
            //  edge closes as sin(theta). Without it a sector's edge would read as far away at the
            //  pole as at the equator, and its softness would fade over a different width at every
            //  latitude.
            return (r.fill * period * 0.5 - std::abs(local)) * std::sin(theta);
        }

        case RegionKind::Dots: {
            const auto d = dotDirections(r.dots);
            if (d.empty()) return -kPi;
            double nearest = -1.0;
            for (const Vec3& v : d) nearest = std::max(nearest, dot(own, v));
            return r.dotSize - std::acos(clampd(nearest, -1.0, 1.0));
        }
    }
    return 0.0;
}

RegionShape sanitised(RegionShape shape) {
    shape.sectors = std::clamp(shape.sectors, 1, kMaxSectors);
    if (dotDirections(shape.dots).empty()) shape.dots = 6;
    shape.seed = std::clamp(shape.seed, 1, kMaxCloudSeed);
    for (double& w : shape.weights)
        if (!std::isfinite(w)) w = 0.0;
    for (double& g : shape.gains) g = std::isfinite(g) ? std::clamp(g, 0.0, 2.0) : 1.0;
    if (shape.kind == RegionKind::Custom) {  // a kind named custom is the flag, over a spot
        shape.kind = RegionKind::Spot;
        shape.custom = true;
    }
    return shape;
}

Region resolveRegion(const RegionShape& shape, RegionSide side, const RegionSettingsDeg& in, double yawTurnRad,
                     double pitchTurnRad, double rollTurnRad) {
    const RegionSettingsDeg def;
    const auto rad = [](double deg, double fallback) { return (std::isfinite(deg) ? deg : fallback) * kDeg2Rad; };
    const auto turn = [](double t) { return std::isfinite(t) ? t : 0.0; };
    const RegionShape safe = sanitised(shape);

    Region r;
    r.kind = safe.custom ? RegionKind::Custom : safe.kind;
    r.side = side;
    r.sectors = safe.sectors;
    r.dots = safe.dots;
    r.yaw = rad(in.yaw, def.yaw) + turn(yawTurnRad);
    r.pitch = rad(in.pitch, def.pitch) + turn(pitchTurnRad);
    r.roll = rad(in.roll, def.roll) + turn(rollTurnRad);
    r.softness = std::max(0.0, rad(in.softness, def.softness));
    r.size = std::clamp(rad(in.size, def.size), 0.0, kPi);
    r.bandElevation = std::clamp(rad(in.bandElevation, def.bandElevation), -kPi * 0.5, kPi * 0.5);
    r.thickness = std::clamp(rad(in.thickness, def.thickness), 0.0, kPi);
    r.fill = std::clamp(std::isfinite(in.fill) ? in.fill : def.fill, 0.0, 1.0);
    r.dotSize = std::clamp(rad(in.dotSize, def.dotSize), 0.0, maxDotSize(safe.dots));
    r.seed = safe.seed;
    //  custom plays weight times its order's gain, folded in here, the one place a shape becomes a region
    for (std::size_t i = 0; i < safe.weights.size(); ++i)
        r.weights[i] = safe.weights[i] * safe.gains[static_cast<std::size_t>(std::sqrt(static_cast<double>(i)))];
    const auto unit = [](double v, double fallback, double hi) {
        return std::clamp(std::isfinite(v) ? v : fallback, 0.0, hi);
    };
    r.coverage = unit(in.coverage, def.coverage, 1.0);
    r.contrast = unit(in.contrast, def.contrast, 2.0);
    r.detail = unit(in.detail, def.detail, 1.0);
    r.evolve = wrapPi(rad(in.evolve, def.evolve));
    return r;
}

double edgeProfile(double d, double softness) {
    const double radius = softness * 0.5;
    if (radius <= 0.0) return d >= 0.0 ? 1.0 : 0.0;
    const double t = clampd(-d / radius, -1.0, 1.0);
    return (std::acos(t) - t * std::sqrt(std::max(0.0, 1.0 - t * t))) / kPi;
}

namespace {
/// The value at a direction already in the region's own frame: the half `valueAt` and a field share.
double weightsValue(std::span<const double> w, Vec3 own) {
    std::array<double, kRegionWeights> y{};
    shSN3D(own, kRegionWeightsOrder, y);
    double v = 0.0;
    for (std::size_t i = 0; i < w.size() && i < y.size(); ++i) v += w[i] * y[i];
    return std::isfinite(v) ? v : 0.0;
}

double valueInOwnFrame(const Region& r, Vec3 own, std::span<const double> weights) {
    double v = 1.0;
    if (isWeightsKind(r.kind)) {
        v = weightsValue(weights, own);
    } else if (r.kind != RegionKind::Everywhere) {
        const double e = edgeDistance(r, own);
        v = r.softness < kHardEdge ? (e >= 0.0 ? 1.0 : 0.0) : edgeProfile(e, r.softness);
        //  A setting that is not a number reads as outside, rather than reaching a smoother that
        //  would hold the NaN until its next reset.
        if (!std::isfinite(v)) v = 0.0;
    }
    return r.side == RegionSide::Outside ? 1.0 - v : v;
}
}  // namespace

double valueAt(const Region& r, Vec3 direction) {
    std::array<double, kRegionWeights> w{};
    if (isWeightsKind(r.kind)) regionWeights(r, kRegionWeightsOrder, w);
    return valueInOwnFrame(r, toOwnFrame(r, unit(direction)), w);
}

RegionField::RegionField(const Region& r)
    : r_(r), x_(toOwnFrame(r, {1, 0, 0})), y_(toOwnFrame(r, {0, 1, 0})), z_(toOwnFrame(r, {0, 0, 1})) {
    if (isWeightsKind(r.kind)) regionWeights(r, kRegionWeightsOrder, weights_);
}

double RegionField::at(Vec3 d) const {
    //  a rotation is linear, so where a direction lands is its components times where the axes land
    return valueInOwnFrame(r_, x_ * d.x + y_ * d.y + z_ * d.z, weights_);
}

}  // namespace bambi
