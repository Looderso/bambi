// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/path/authoring.hpp"

#include <algorithm>
#include <cmath>

#include "bambi/math/sphere.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"

namespace bambi {
namespace {

Vec3 lerp3(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }

/*  Fitting clamp, span-relative — NOT the interactive one.
 *
 *  A handle longer than its own segment guarantees a cusp, and because playback walks by
 *  arc length the backtrack is faithfully reproduced as a stall. Interactive editing keeps
 *  the absolute floor on purpose: a long handle you dragged yourself is a shape, not a bug.
 */
double fitClamp(double v, double span) {
    const double lo = std::min(kHandleMinRad, span * 0.25);
    const double hi = std::min(kHandleMaxRad, span * 0.9);
    return std::clamp(v, lo, hi);
}

Node makeNode(Vec3 p, Vec3 tangent, double lenIn, double lenOut) {
    const Vec3 t = unit(tangentAt(p, tangent));
    Node n;
    n.p = p;
    n.cin = expMap(p, t * -std::max(lenIn, 1e-4));
    n.cout = expMap(p, t * std::max(lenOut, 1e-4));
    n.smooth = true;
    return n;
}

/// Signed geodesic curvature along a polyline, box-smoothed because it is noisy.
std::vector<double> signedCurvature(const std::vector<Vec3>& pts, bool closed) {
    const std::size_t n = pts.size();
    std::vector<double> k(n, 0.0), sm(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t ia = closed ? (i + n - 1) % n : (i == 0 ? 0 : i - 1);
        const std::size_t ib = closed ? (i + 1) % n : std::min(i + 1, n - 1);
        const Vec3 t1 = unit(tangentAt(pts[i], pts[i] - pts[ia]));
        const Vec3 t2 = unit(tangentAt(pts[i], pts[ib] - pts[i]));
        const double ang = arc(t1, t2);
        k[i] = dot(cross(t1, t2), pts[i]) < 0 ? -ang : ang;
    }
    constexpr int W = 3;
    for (std::size_t i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int d = -W; d <= W; ++d) {
            const auto j =
                closed ? (i + static_cast<std::size_t>(d + static_cast<int>(n))) % n
                       : static_cast<std::size_t>(std::clamp(static_cast<int>(i) + d, 0, static_cast<int>(n) - 1));
            acc += k[j];
        }
        sm[i] = acc / (2 * W + 1);
    }
    return sm;
}

std::vector<std::size_t> featureIndices(const std::vector<Vec3>& pts, int targetN, bool closed) {
    const std::size_t n = pts.size();
    const std::vector<double> k = signedCurvature(pts, closed);

    /*  A geodesic has zero curvature, so k is pure numerical noise oscillating about zero
     *  and every sample looks like an extremum. Without a floor relative to the path's own
     *  curvature scale, a straight arc gets nodes at random noise positions — one segment a
     *  fraction of a degree long, which then guarantees a cusp. */
    std::vector<double> mag(n);
    for (std::size_t i = 0; i < n; ++i) mag[i] = std::abs(k[i]);
    std::sort(mag.begin(), mag.end());
    const double scale = mag[static_cast<std::size_t>(static_cast<double>(n) * 0.9)];
    const double floorK = std::max(scale * 0.05, 1e-6);

    std::vector<std::size_t> cand;
    const std::size_t minGap = std::max<std::size_t>(2, n / static_cast<std::size_t>(std::max(targetN * 3, 1)));
    const std::size_t lo = closed ? 0 : 1;
    const std::size_t hi = closed ? n : n - 1;
    for (std::size_t i = lo; i < hi; ++i) {
        const double prev = k[(i + n - 1) % n], next = k[(i + 1) % n];
        const bool extremum = (k[i] - prev) * (next - k[i]) < 0 && std::abs(k[i]) > floorK;
        const bool inflect = prev * next < 0 && std::max(std::abs(prev), std::abs(next)) > floorK;
        if ((extremum || inflect) && (cand.empty() || i - cand.back() >= minGap)) cand.push_back(i);
    }

    if (!closed)
        cand.erase(std::remove_if(cand.begin(), cand.end(), [&](std::size_t i) { return i == 0 || i + 1 >= n; }),
                   cand.end());

    const int keep = targetN - (closed ? 0 : 2);
    if (keep > 0 && static_cast<int>(cand.size()) > keep) {
        std::vector<std::size_t> thinned;
        const double step = static_cast<double>(cand.size()) / keep;
        for (int i = 0; i < keep; ++i) thinned.push_back(cand[static_cast<std::size_t>(static_cast<double>(i) * step)]);
        cand = thinned;
    }
    if (!closed) {
        cand.insert(cand.begin(), 0);
        cand.push_back(n - 1);
    }

    if (cand.size() < 3) {  // constant curvature (a circle, a geodesic): no features at all
        cand.clear();
        const double spanN = static_cast<double>(closed ? n : n - 1);
        const double divN = static_cast<double>(closed ? targetN : std::max(targetN - 1, 1));
        for (int i = 0; i < targetN; ++i)
            cand.push_back(static_cast<std::size_t>(std::llround(static_cast<double>(i) * spanN / divN)));
    }

    // Drop runts: a surviving noise feature beside a forced endpoint leaves two nodes a
    // fraction of a degree apart, which fits fine but looks broken.
    if (cand.size() > 2) {
        const double avg = static_cast<double>(n) / targetN;
        std::vector<std::size_t> kept{cand.front()};
        for (std::size_t i = 1; i < cand.size(); ++i) {
            const bool mustKeep = !closed && i + 1 == cand.size();
            if (mustKeep) {
                while (kept.size() > 1 && static_cast<double>(cand[i] - kept.back()) < avg * 0.35) kept.pop_back();
                kept.push_back(cand[i]);
            } else if (static_cast<double>(cand[i] - kept.back()) >= avg * 0.35) {
                kept.push_back(cand[i]);
            }
        }
        if (kept.size() >= 2) cand = kept;
    }

    while (static_cast<int>(cand.size()) < targetN) {  // fill the widest gap
        long worst = -1;
        std::size_t at = 0;
        const std::size_t lim = closed ? cand.size() : cand.size() - 1;
        for (std::size_t i = 0; i < lim; ++i) {
            const std::size_t a = cand[i], b = cand[(i + 1) % cand.size()];
            const long span = closed ? static_cast<long>((b + n - a) % n) : static_cast<long>(b - a);
            if (span > worst) {
                worst = span;
                at = i;
            }
        }
        if (worst < 4) break;
        cand.insert(cand.begin() + static_cast<long>(at) + 1, (cand[at] + static_cast<std::size_t>(worst / 2)) % n);
        std::sort(cand.begin(), cand.end());
    }

    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    return cand;
}

/*  One-sided Hausdorff of a candidate segment against the reference arc, plus a
 *  monotonicity penalty.
 *
 *  Distance alone is under-constrained: on a straight or low-curvature segment many handle
 *  combinations trace exactly the right path while the parameterisation reverses — the
 *  curve runs forward, backs up, continues. Geometric error is zero, so an unpenalised
 *  optimiser happily picks one, and playback then reproduces the backtrack as a stall.
 */
double segError(const Vec3 (&P)[4], const std::vector<Vec3>& ref) {
    double worst = 0.0, penalty = 0.0;
    long prevIdx = -1;
    for (int s = 1; s < 12; ++s) {
        const Vec3 q = unit(cubicBezier(P, s / 12.0));
        double m = 1e9;
        long mi = 0;
        for (std::size_t r = 0; r < ref.size(); ++r) {
            const double d = arc(q, ref[r]);
            if (d < m) {
                m = d;
                mi = static_cast<long>(r);
            }
        }
        worst = std::max(worst, m);
        if (mi < prevIdx) penalty += static_cast<double>(prevIdx - mi) / static_cast<double>(ref.size());
        prevIdx = mi;
    }
    return worst + penalty;
}

constexpr double kScales[] = {0.14, 0.20, 0.26, 0.32, 0.38, 0.45, 0.52, 0.60};

std::size_t segmentCount(const TrajectoryState& s) {
    if (s.nodes.size() < 2) return 0;
    return s.closed ? s.nodes.size() : s.nodes.size() - 1;
}

double handleLength(const TrajectoryState& s, std::size_t index, Handle which) {
    if (index >= s.nodes.size()) return 0.0;
    const Node& n = s.nodes[index];
    return arc(n.p, which == Handle::In ? n.cin : n.cout);
}

Vec3 handleDirection(const TrajectoryState& s, std::size_t index, Handle which) {
    if (index >= s.nodes.size()) return {1, 0, 0};
    const Node& n = s.nodes[index];
    return unit(logMap(n.p, which == Handle::In ? n.cin : n.cout));
}

}  // namespace

// ---- queries ---------------------------------------------------------------------------

bool handleIsLive(const TrajectoryState& s, std::size_t index, Handle which) {
    if (index >= s.nodes.size()) return false;
    if (s.closed) return true;
    return which == Handle::In ? index > 0 : index + 1 < s.nodes.size();
}

// ---- node operations -------------------------------------------------------------------

std::size_t insertNode(TrajectoryState& s, std::size_t segment, double t) {
    if (segment >= segmentCount(s)) return s.nodes.size();
    //  Refused at the cap with the same answer as an out-of-range segment, so a caller that
    //  already handles one refusal handles this one too.
    if (s.nodes.size() >= static_cast<std::size_t>(kMaxNodes)) return s.nodes.size();
    t = clampd(t, 0.0, 1.0);

    Vec3 P[4];
    segmentControls(s.nodes, segment, P);
    const Vec3 A = lerp3(P[0], P[1], t), B = lerp3(P[1], P[2], t), C = lerp3(P[2], P[3], t);
    const Vec3 D = lerp3(A, B, t), E = lerp3(B, C, t);
    const Vec3 F = lerp3(D, E, t);  // == the curve point at t

    const std::size_t j = (segment + 1) % s.nodes.size();
    s.nodes[segment].cout = unit(A);
    s.nodes[j].cin = unit(C);

    Node n;
    n.p = unit(F);
    n.cin = unit(D);
    n.cout = unit(E);
    n.smooth = true;
    s.nodes.insert(s.nodes.begin() + static_cast<long>(segment) + 1, n);
    return segment + 1;
}

bool deleteNode(TrajectoryState& s, std::size_t index) {
    if (index >= s.nodes.size() || s.nodes.size() <= 2) return false;
    s.nodes.erase(s.nodes.begin() + static_cast<long>(index));
    return true;
}

void moveNode(TrajectoryState& s, std::size_t index, Vec3 to) {
    if (index >= s.nodes.size()) return;
    Node& n = s.nodes[index];
    const Vec3 q = unit(to);
    n.cin = unit(rotateAToB(n.cin, n.p, q));
    n.cout = unit(rotateAToB(n.cout, n.p, q));
    n.p = q;
}

void setHandle(TrajectoryState& s, std::size_t index, Handle which, Vec3 target) {
    if (index >= s.nodes.size()) return;
    Node& n = s.nodes[index];
    const Vec3 dir = unit(tangentAt(n.p, unit(target) - n.p));
    const double len = std::clamp(arc(n.p, unit(target)), kHandleMinRad, kHandleMaxRad);
    (which == Handle::In ? n.cin : n.cout) = expMap(n.p, dir * len);
    if (n.smooth) {
        const double other = std::clamp(handleLength(s, index, which == Handle::In ? Handle::Out : Handle::In),
                                        kHandleMinRad, kHandleMaxRad);
        (which == Handle::In ? n.cout : n.cin) = expMap(n.p, dir * -other);
    }
}

void makeSmooth(TrajectoryState& s, std::size_t index) {
    if (index >= s.nodes.size()) return;
    Node& n = s.nodes[index];
    const Vec3 d = handleDirection(s, index, Handle::Out);
    const double li = std::clamp(handleLength(s, index, Handle::In), kHandleMinRad, kHandleMaxRad);
    n.smooth = true;
    n.cin = expMap(n.p, d * -li);
}

void makeCorner(TrajectoryState& s, std::size_t index) {
    if (index < s.nodes.size()) s.nodes[index].smooth = false;
}

void makeStart(TrajectoryState& s, std::size_t index) {
    if (index == 0 || index >= s.nodes.size()) return;
    std::rotate(s.nodes.begin(), s.nodes.begin() + static_cast<long>(index), s.nodes.end());
}

void setClosed(TrajectoryState& s, bool closed) { s.closed = closed; }

// ---- conversion ------------------------------------------------------------------------

FitResult fitNodes(TrajectoryState& s, const std::vector<Vec3>& dense, int targetNodes, bool closed) {
    FitResult r;
    //  The one funnel every fit goes through -- convert, reset, refit on load -- so the cap
    //  is enforced once, here.
    targetNodes = targetNodes > kMaxNodes ? kMaxNodes : targetNodes;
    if (dense.size() < 4 || targetNodes < 2) return r;

    const std::size_t n = dense.size();
    const std::vector<std::size_t> idx = featureIndices(dense, targetNodes, closed);
    const std::size_t N = idx.size();
    if (N < 2) return r;

    std::vector<Node> built;
    built.reserve(N);
    for (std::size_t j = 0; j < N; ++j) {
        const std::size_t i = idx[j];
        const std::size_t ia = closed ? (i + n - 1) % n : (i == 0 ? 0 : i - 1);
        const std::size_t ib = closed ? (i + 1) % n : std::min(i + 1, n - 1);
        const double span = std::max(arc(dense[i], dense[idx[(j + 1) % N]]), 0.1);
        built.push_back(makeNode(dense[i], dense[ib] - dense[ia], fitClamp(span / 3, span), fitClamp(span / 3, span)));
    }

    const std::size_t nSeg = closed ? N : N - 1;
    double maxDev = 0.0;
    for (std::size_t j = 0; j < nSeg; ++j) {
        Node& a = built[j];
        Node& b = built[(j + 1) % N];
        const std::size_t i0 = idx[j], i1 = idx[(j + 1) % N];

        std::vector<Vec3> ref;
        for (std::size_t i = i0; i != i1; i = (i + 1) % n) ref.push_back(dense[i]);
        ref.push_back(dense[i1]);
        if (ref.size() < 3) continue;

        const double span = std::max(arc(a.p, b.p), 0.1);
        const Vec3 dOut = unit(logMap(a.p, a.cout));
        const Vec3 dIn = unit(logMap(b.p, b.cin));

        auto trial = [&](double so, double si) {
            const Vec3 P[4] = {a.p, expMap(a.p, dOut * (so * span)), expMap(b.p, dIn * (si * span)), b.p};
            return segError(P, ref);
        };

        double bestE = 1e18, bestSo = 0.33, bestSi = 0.33;
        for (double so : kScales)
            for (double si : kScales) {
                const double e = trial(so, si);
                if (e < bestE) {
                    bestE = e;
                    bestSo = so;
                    bestSi = si;
                }
            }
        // The coarse grid leaves a quantisation floor that more nodes cannot fix, because
        // the residual is handle length rather than placement. Refine locally instead.
        double step = 0.06;
        for (int pass = 0; pass < 3; ++pass, step /= 3.0) {
            for (int da = -1; da <= 1; ++da)
                for (int db = -1; db <= 1; ++db) {
                    if (da == 0 && db == 0) continue;
                    const double so = std::clamp(bestSo + da * step, 0.05, 0.95);
                    const double si = std::clamp(bestSi + db * step, 0.05, 0.95);
                    const double e = trial(so, si);
                    if (e < bestE) {
                        bestE = e;
                        bestSo = so;
                        bestSi = si;
                    }
                }
        }

        a.cout = expMap(a.p, dOut * fitClamp(bestSo * span, span));
        b.cin = expMap(b.p, dIn * fitClamp(bestSi * span, span));
        maxDev = std::max(maxDev, bestE);
    }

    s.kind = TrajectoryKind::Custom;
    s.closed = closed;
    s.nodes = std::move(built);
    r.nodes = s.nodes.size();
    r.maxDeviationRad = maxDev;
    return r;
}

//  Points a curve is sampled at before a fit.
constexpr int kFitSamples = 1500;

FitResult convertToCustom(TrajectoryState& s, int targetNodes) {
    const bool closed = s.kind == TrajectoryKind::Parametric ? generatorIsClosed(s.generator) : s.closed;
    std::vector<Vec3> dense;
    dense.reserve(kFitSamples);
    if (s.kind == TrajectoryKind::Parametric) {
        for (int i = 0; i < kFitSamples; ++i) {
            const double u = closed ? static_cast<double>(i) / kFitSamples : static_cast<double>(i) / (kFitSamples - 1);
            dense.push_back(generatorAt(s.generator, u, s.genParams));
        }
    } else {
        for (const auto& sample : samplePath(s)) dense.push_back(sample.p);
    }
    return fitNodes(s, dense, targetNodes, closed);
}

}  // namespace bambi
