// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/fill.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "bambi/dsp/fft.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"

namespace bambi {

namespace {

std::size_t sz(int n) { return static_cast<std::size_t>(n); }
using Cx = std::complex<double>;

//  The fill's times, in samples at 48 kHz; other rates keep them in time.
constexpr int kBulk48k = 32;    // the binaural filters' bulk delay
constexpr int kDelay48k = 128;  // the cancellation's delay, and the dry part's
constexpr int kTaps48k = 384;   // the cancellation filters' length
constexpr int kFade48k = 64;    // the half-Hann over their last taps
constexpr int kFft48k = 2048;   // the cancellation is designed on this many bins and cut to its length

int atRate(int samples48k, double sampleRate) {
    return std::max(1, static_cast<int>(std::lround(samples48k * sampleRate / 48000.0)));
}

/// The cosine of the angle from p to the shorter great-circle arc from a to b.
double cosToArc(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 n = cross(a, b);
    const double ln = length(n);
    if (ln > 1e-12) {
        const Vec3 u = n * (1.0 / ln);
        const double s = dot(p, u);
        const Vec3 q = p - u * s;  // p on the arc's great circle
        if (length(q) > 1e-12 && dot(cross(a, q), u) >= 0.0 && dot(cross(q, b), u) >= 0.0)
            return std::sqrt(std::max(0.0, 1.0 - s * s));
    }
    return std::max(dot(p, a), dot(p, b));
}

/// p inside the spherical triangle a b c, wound outward.
bool inside(Vec3 p, const std::array<Vec3, 3>& t) {
    return dot(p, cross(t[0], t[1])) >= 0.0 && dot(p, cross(t[1], t[2])) >= 0.0 && dot(p, cross(t[2], t[0])) >= 0.0;
}

double smoothStep(double a, double b, double x) {
    const double t = clampd((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

int fillCancellationDelay(double sampleRate) { return atRate(kDelay48k, sampleRate); }

FilterDesign fillFilterDesign(double sampleRate) {
    FilterDesign d;
    d.order = kFillOrder;
    d.sampleRate = sampleRate;
    d.delay = atRate(kBulk48k, sampleRate);
    return d;
}

CoveredRegion coveredRegion(const SpeakerDesign& design) {
    CoveredRegion r;
    const int n = static_cast<int>(design.points.size());
    const auto real = [&](int i) { return !design.imaginary[sz(i)]; };
    for (int i = 0; i < n; ++i)
        if (real(i)) r.points.push_back(design.points[sz(i)]);
    const int realCount = static_cast<int>(r.points.size());
    if (realCount == 0) return r;
    r.points.clear();

    if (!design.triangles.empty()) {
        //  A centre is covered when every corner it shares its gain with is a real loudspeaker.
        std::vector<bool> covered(sz(n));
        for (int i = 0; i < n; ++i) covered[sz(i)] = real(i);
        for (int i = 0; i < n; ++i) {
            if (!design.centre[sz(i)]) continue;
            bool all = true;
            for (const std::array<int, 3>& t : design.triangles)
                if (t[0] == i || t[1] == i || t[2] == i)
                    for (int v : t) all = all && (v == i || real(v));
            covered[sz(i)] = all;
        }
        //  The covered triangles, and their outer edges: those only one of them has.
        r.sphere = true;
        std::vector<std::array<int, 2>> edges;
        for (const std::array<int, 3>& t : design.triangles) {
            if (!(covered[sz(t[0])] && covered[sz(t[1])] && covered[sz(t[2])])) {
                r.sphere = false;
                continue;
            }
            r.triangles.push_back({design.points[sz(t[0])], design.points[sz(t[1])], design.points[sz(t[2])]});
            for (int e = 0; e < 3; ++e)
                edges.push_back({std::min(t[sz(e)], t[sz((e + 1) % 3)]), std::max(t[sz(e)], t[sz((e + 1) % 3)])});
        }
        std::sort(edges.begin(), edges.end());
        for (std::size_t k = 0; k < edges.size(); ++k) {
            const bool shared =
                (k + 1 < edges.size() && edges[k + 1] == edges[k]) || (k > 0 && edges[k - 1] == edges[k]);
            if (!shared) r.arcs.push_back({design.points[sz(edges[k][0])], design.points[sz(edges[k][1])]});
        }
        for (int i = 0; i < n; ++i) {
            bool onOne = false;
            for (const std::array<int, 2>& e : edges) onOne = onOne || e[0] == i || e[1] == i;
            if (real(i) && !onOne) r.points.push_back(design.points[sz(i)]);
        }
    } else if (realCount == 2) {
        std::vector<Vec3> two;
        for (int i = 0; i < n; ++i)
            if (real(i)) two.push_back(design.points[sz(i)]);
        r.arcs.push_back({two[0], two[1]});
    } else if (design.ring) {
        //  The arcs between neighbours by azimuth, both real; a ring's design never leaves a gap past half a turn.
        std::vector<int> order(sz(n));
        for (int i = 0; i < n; ++i) order[sz(i)] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return azimuth(design.points[sz(a)]) < azimuth(design.points[sz(b)]); });
        for (int k = 0; k < n; ++k) {
            const int a = order[sz(k)], b = order[sz((k + 1) % n)];
            if (real(a) && real(b)) r.arcs.push_back({design.points[sz(a)], design.points[sz(b)]});
        }
        for (int k = 0; k < n; ++k) {
            const int i = order[sz(k)];
            if (real(i) && !real(order[sz((k + 1) % n)]) && !real(order[sz((k + n - 1) % n)]))
                r.points.push_back(design.points[sz(i)]);
        }
    } else {
        for (int i = 0; i < n; ++i)
            if (real(i)) r.points.push_back(design.points[sz(i)]);
    }
    return r;
}

double angleToRegion(const CoveredRegion& region, Vec3 direction) {
    if (region.sphere) return 0.0;
    for (const std::array<Vec3, 3>& t : region.triangles)
        if (inside(direction, t)) return 0.0;
    double best = -1.0;  // the nearest cosine
    for (const std::array<Vec3, 2>& a : region.arcs) best = std::max(best, cosToArc(direction, a[0], a[1]));
    for (const Vec3& p : region.points) best = std::max(best, dot(direction, p));
    return std::acos(clampd(best, -1.0, 1.0));
}

namespace {

/// The dry and binaural gains of a plane wave from `direction`.
std::array<double, 2> fillGains(const CoveredRegion& region, const FillSettings& settings, Vec3 direction) {
    const double d = angleToRegion(region, direction) * kRad2Deg - settings.holdDeg;
    double f = 1.0;
    if (d > 0.0) f = d >= settings.fadeDeg ? 0.0 : std::pow(std::cos(0.5 * kPi * d / settings.fadeDeg), 2.0);
    if (settings.equalPower) return {std::sqrt(f), std::sqrt(1.0 - f)};
    return {f, 1.0 - f};
}

}  // namespace

void fillWindows(const CoveredRegion& region, const FillSettings& settings, int order, FillWindows& out) {
    std::vector<Vec3> dirs;
    std::vector<double> w;
    quadratureGrid(32, 64, dirs, w);
    const int C = numChannels(order), H = numChannels(kFillOrder);
    std::vector<double> y(sz(numChannels(std::max(order, kFillOrder))));
    out.order = order;
    out.dry.assign(sz(C) * sz(C), 0.0);
    out.hrtf.assign(sz(H) * sz(C), 0.0);
    for (std::size_t g = 0; g < dirs.size(); ++g) {
        shSN3D(dirs[g], std::max(order, kFillOrder), y);
        const std::array<double, 2> gains = fillGains(region, settings, dirs[g]);
        const double a = w[g] * gains[0], b = w[g] * gains[1];
        for (int r = 0; r < C; ++r)
            for (int c = 0; c < C; ++c) out.dry[sz(r) * sz(C) + sz(c)] += a * y[sz(r)] * y[sz(c)];
        for (int r = 0; r < H; ++r)
            for (int c = 0; c < C; ++c) out.hrtf[sz(r) * sz(C) + sz(c)] += b * y[sz(r)] * y[sz(c)];
    }
    //  SN3D: Y_c squared averages 1 / (2n + 1) over the sphere, so the projection gives each row that back.
    for (int r = 0; r < C; ++r)
        for (int c = 0; c < C; ++c) out.dry[sz(r) * sz(C) + sz(c)] *= 2.0 * acnOrder(r) + 1.0;
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < C; ++c) out.hrtf[sz(r) * sz(C) + sz(c)] *= 2.0 * acnOrder(r) + 1.0;
}

SpeakerDesign windowedDecoder(const SpeakerDesign& design, const FillWindows& windows) {
    const int C = numChannels(windows.order);
    SpeakerDesign out = design;
    std::fill(out.matrix.begin(), out.matrix.end(), 0.0);
    for (int o = 0; o < design.outputs; ++o)
        for (int r = 0; r < C; ++r) {
            const double d = design.matrix[sz(o) * sz(C) + sz(r)];
            if (d == 0.0) continue;
            for (int c = 0; c < C; ++c) out.matrix[sz(o) * sz(C) + sz(c)] += d * windows.dry[sz(r) * sz(C) + sz(c)];
        }
    return out;
}

bool designCancellation(const SpeakerDesign& design, const BinauralFilterBuilder& head, const FillSettings& settings,
                        double sampleRate, Cancellation& out, std::string* error) {
    out = Cancellation{};
    std::vector<Vec3> at;
    for (std::size_t i = 0; i < design.points.size(); ++i)
        if (!design.imaginary[i]) at.push_back(design.points[i]);
    out.outputs = design.channel;
    const int L = static_cast<int>(at.size());
    const auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (L < 2) return fail("crosstalk cancellation needs two loudspeakers or more");
    if (head.channels() != numChannels(kFillOrder)) return fail("the binaural filters are not of the fill's order");

    const int taps = atRate(kTaps48k, sampleRate), D = fillCancellationDelay(sampleRate);
    const int bulk = fillFilterDesign(sampleRate).delay, fade = atRate(kFade48k, sampleRate);
    int n = kFft48k;
    while (n < kFft48k * sampleRate / 48000.0) n *= 2;
    RealFft fft;
    if (!fft.prepare(sz(n))) return fail("no FFT of that size");
    const int bins = n / 2 + 1;
    out.taps = taps;
    out.delay = D;

    //  The head's response from each loudspeaker to each ear: the binaural filters at the loudspeaker's direction.
    std::vector<Cx> C(sz(2) * sz(L) * sz(bins));  // [ear][loudspeaker][bin]
    {
        std::vector<double> y(sz(numChannels(kFillOrder))), h(sz(n)), re(sz(bins)), im(sz(bins));
        for (int i = 0; i < L; ++i) {
            shSN3D(at[sz(i)], kFillOrder, y);
            for (int e = 0; e < 2; ++e) {
                std::fill(h.begin(), h.end(), 0.0);
                for (int c = 0; c < head.channels(); ++c) {
                    const std::span<const float> f = head.filter(c, e);
                    for (int t = 0; t < head.taps() && t < n; ++t) h[sz(t)] += y[sz(c)] * f[sz(t)];
                }
                fft.forward(h, re, im);
                for (int k = 0; k < bins; ++k) C[(sz(e) * sz(L) + sz(i)) * sz(bins) + sz(k)] = {re[sz(k)], im[sz(k)]};
            }
        }
    }
    const auto c = [&](int e, int i, int k) { return C[(sz(e) * sz(L) + sz(i)) * sz(bins) + sz(k)]; };

    //  Outside the band each ear's channel plays uncancelled on one loudspeaker: of those on its side of the listener
    //  (of all, when its side has none), the nearest to 30 degrees to that side at ear level -- for a pair, its own
    //  side's loudspeaker, as a stereo pair plays it.
    std::vector<double> plain(sz(L) * 2, 0.0);  // [loudspeaker][ear]
    for (int e = 0; e < 2; ++e) {
        const Vec3 aim = fromAzEl((e == 0 ? 30.0 : -30.0) * kDeg2Rad, 0.0);
        const auto onSide = [&](int i) { return e == 0 ? at[sz(i)].y > 1e-6 : at[sz(i)].y < -1e-6; };
        bool any = false;
        for (int i = 0; i < L; ++i) any = any || onSide(i);
        int best = -1;
        for (int i = 0; i < L; ++i)
            if ((!any || onSide(i)) && (best < 0 || dot(at[sz(i)], aim) > dot(at[sz(best)], aim))) best = i;
        plain[sz(best) * 2 + sz(e)] = 1.0;
    }

    //  Per bin, the least-energy loudspeaker signals that give each ear its own channel D samples late:
    //  G = C^H (C C^H + beta tr / 2)^-1, then blended with the plain feed outside the band.
    const double beta = std::pow(10.0, settings.regularisationDb / 10.0);
    const double nyquist = 0.5 * sampleRate;
    const double lo0 = 0.5 * settings.bandLoHz, lo1 = settings.bandLoHz;
    const double hi0 = settings.bandHiHz, hi1 = std::min(1.4 * settings.bandHiHz, 0.95 * nyquist);
    std::vector<Cx> G(sz(L) * 2 * sz(bins));  // [loudspeaker][ear][bin]
    const double betaRef = std::pow(10.0, kFillReferenceDb / 10.0);
    double referenceRatio = 0.0;  // what the layout can reach: the inversion exact and regularised at the reference
    int referenceCounted = 0;
    for (int k = 0; k < bins; ++k) {
        Cx A[2][2] = {};
        for (int l = 0; l < 2; ++l)
            for (int m = 0; m < 2; ++m)
                for (int i = 0; i < L; ++i) A[l][m] += c(l, i, k) * std::conj(c(m, i, k));
        const double hzRef = k * sampleRate / n;
        if (hzRef >= lo1 && hzRef <= hi0) {
            const double r = betaRef * 0.5 * (A[0][0].real() + A[1][1].real());
            const Cx a00 = A[0][0] + r, a11 = A[1][1] + r, d = a00 * a11 - A[0][1] * A[1][0];
            const Cx ri[2][2] = {{a11 / d, -A[0][1] / d}, {-A[1][0] / d, a00 / d}};
            Cx E[2][2] = {};
            for (int l = 0; l < 2; ++l)
                for (int m = 0; m < 2; ++m)
                    for (int i = 0; i < L; ++i) {
                        Cx g = 0.0;
                        for (int j = 0; j < 2; ++j) g += std::conj(c(j, i, k)) * ri[j][m];
                        E[l][m] += c(l, i, k) * g;
                    }
            referenceRatio += (std::norm(E[0][1]) + std::norm(E[1][0])) / (std::norm(E[0][0]) + std::norm(E[1][1]));
            ++referenceCounted;
        }
        const double reg = beta * 0.5 * (A[0][0].real() + A[1][1].real());
        A[0][0] += reg;
        A[1][1] += reg;
        const Cx det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
        const Cx inv[2][2] = {{A[1][1] / det, -A[0][1] / det}, {-A[1][0] / det, A[0][0] / det}};
        const double hz = k * sampleRate / n;
        const double inBand = smoothStep(lo0, lo1, hz) * (1.0 - smoothStep(hi0, hi1, hz));
        const Cx late = std::polar(1.0, -2.0 * kPi * k * D / n),
                 plainLate = std::polar(1.0, -2.0 * kPi * k * (D - bulk) / n);
        for (int i = 0; i < L; ++i)
            for (int m = 0; m < 2; ++m) {
                Cx g = 0.0;
                for (int l = 0; l < 2; ++l) g += std::conj(c(l, i, k)) * inv[l][m];
                G[(sz(i) * 2 + sz(m)) * sz(bins) + sz(k)] =
                    inBand * g * late + (1.0 - inBand) * plain[sz(i) * 2 + sz(m)] * plainLate;
            }
    }

    //  To filters: the response cut to its length, the last taps faded. What rang ahead of time 0 wrapped to the
    //  end of the transform and is cut with it.
    out.filters.assign(sz(2) * sz(L) * sz(taps), 0.0f);
    {
        std::vector<double> re(sz(bins)), im(sz(bins)), x(sz(n));
        for (int i = 0; i < L; ++i)
            for (int m = 0; m < 2; ++m) {
                for (int k = 0; k < bins; ++k) {
                    const Cx g = G[(sz(i) * 2 + sz(m)) * sz(bins) + sz(k)];
                    re[sz(k)] = g.real();
                    im[sz(k)] = g.imag();
                }
                fft.inverse(re, im, x);
                float* f = out.filters.data() + (sz(m) * sz(L) + sz(i)) * sz(taps);
                for (int t = 0; t < taps; ++t) {
                    const int left = taps - 1 - t;
                    const double w = left < fade ? std::pow(std::sin(0.5 * kPi * left / fade), 2.0) : 1.0;
                    f[t] = static_cast<float>(x[sz(t)] * w);
                }
            }
    }

    //  What the filters as cut give the two ears, with the same head, over the band.
    {
        std::vector<double> x(sz(n)), re(sz(bins)), im(sz(bins));
        std::vector<Cx> Gc(sz(L) * 2 * sz(bins));
        for (int i = 0; i < L; ++i)
            for (int m = 0; m < 2; ++m) {
                std::fill(x.begin(), x.end(), 0.0);
                const float* f = out.filters.data() + (sz(m) * sz(L) + sz(i)) * sz(taps);
                for (int t = 0; t < taps; ++t) x[sz(t)] = f[t];
                fft.forward(x, re, im);
                for (int k = 0; k < bins; ++k) Gc[(sz(i) * 2 + sz(m)) * sz(bins) + sz(k)] = {re[sz(k)], im[sz(k)]};
            }
        double ratio = 0.0;
        int counted = 0;
        for (int k = 0; k < bins; ++k) {
            const double hz = k * sampleRate / n;
            if (hz < lo1 || hz > hi0) continue;
            Cx E[2][2] = {};
            for (int l = 0; l < 2; ++l)
                for (int m = 0; m < 2; ++m)
                    for (int i = 0; i < L; ++i) E[l][m] += c(l, i, k) * Gc[(sz(i) * 2 + sz(m)) * sz(bins) + sz(k)];
            ratio += (std::norm(E[0][1]) + std::norm(E[1][0])) / (std::norm(E[0][0]) + std::norm(E[1][1]));
            ++counted;
        }
        out.separationDb = 10.0 * std::log10(std::max(ratio / std::max(counted, 1), 1e-30));

        //  The level: for a plane wave from each loudspeaker, the energy the fill's feeds carry, mean over the
        //  audible bins, against what the layout's decoder plays for it. One gain for all, so the fill and the
        //  decoder part meet at the loudspeakers at one level, at any rate.
        std::vector<double> y(sz(numChannels(design.order)));
        double decoded = 0.0, filled = 0.0;
        const int Cd = numChannels(design.order);
        const int audible = std::min(bins, static_cast<int>(20000.0 * n / sampleRate) + 1);
        for (int i = 0; i < L; ++i) {
            shSN3D(at[sz(i)], design.order, y);
            for (int o = 0; o < design.outputs; ++o) {
                double gain = 0.0;
                for (int ch = 0; ch < Cd; ++ch) gain += design.matrix[sz(o) * sz(Cd) + sz(ch)] * y[sz(ch)];
                decoded += gain * gain;
            }
        }
        //  Summed over the loudspeakers the plane waves come from and the feeds they reach, per bin
        //  sum_ij |sum_m Gc[j][m] C[m][i]|^2 = sum_mn (sum_j Gc[j][m] Gc[j][n]*) (sum_i C[m][i] C[n][i]*).
        for (int k = 0; k < audible; ++k) {
            Cx S[2][2] = {}, A[2][2] = {};
            for (int j = 0; j < L; ++j)
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q) {
                        S[m][q] += Gc[(sz(j) * 2 + sz(m)) * sz(bins) + sz(k)] *
                                   std::conj(Gc[(sz(j) * 2 + sz(q)) * sz(bins) + sz(k)]);
                        A[m][q] += c(m, j, k) * std::conj(c(q, j, k));
                    }
            for (int m = 0; m < 2; ++m)
                for (int q = 0; q < 2; ++q) filled += (S[m][q] * A[m][q]).real();
        }
        filled /= audible;
        out.level = filled > 0.0 ? std::sqrt(decoded / filled) : 1.0;
        for (float& v : out.filters) v = static_cast<float>(v * out.level);
    }
    //  Each loudspeaker's distance gain and delay, as the speaker renderer gives the dry part, folded into its
    //  feeds: designed for loudspeakers that arrive together, they reach a room that is not a sphere the same way.
    int longest = 0;
    std::vector<int> late(sz(L), 0);
    for (int i = 0; i < L; ++i) {
        late[sz(i)] = sz(i) < design.delaySeconds.size()
                          ? static_cast<int>(std::lround(design.delaySeconds[sz(i)] * sampleRate))
                          : 0;
        longest = std::max(longest, late[sz(i)]);
    }
    if (longest > 0 || !design.gain.empty()) {
        std::vector<float> placed(sz(2) * sz(L) * sz(taps + longest), 0.0f);
        for (int m = 0; m < 2; ++m)
            for (int i = 0; i < L; ++i) {
                const float g = sz(i) < design.gain.size() ? static_cast<float>(design.gain[sz(i)]) : 1.0f;
                const float* from = out.filters.data() + (sz(m) * sz(L) + sz(i)) * sz(taps);
                float* to = placed.data() + (sz(m) * sz(L) + sz(i)) * sz(taps + longest) + sz(late[sz(i)]);
                for (int t = 0; t < taps; ++t) to[t] = g * from[t];
            }
        out.filters.swap(placed);
        out.taps = taps + longest;
    }
    const double reference = 10.0 * std::log10(std::max(referenceRatio / std::max(referenceCounted, 1), 1e-30));
    if (reference > kFillSeparationNeededDb) {
        const long tenths = std::lround(-reference * 10.0);
        const std::string got = std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
        const std::string need = std::to_string(static_cast<int>(-kFillSeparationNeededDb));
        if (error)
            *error = "crosstalk cancellation fails for this layout: separation " + got + " dB, " + need + " dB needed";
        return false;
    }
    return true;
}

double fillLoudness(const SpeakerDesign& design, const FillWindows& windows, const Cancellation& cancel,
                    const BinauralFilterBuilder& head, double sampleRate) {
    //  Averaged over the sphere a field of SN3D harmonics has energy 1 / (2n + 1) in each channel of degree n and
    //  none shared between channels, so each part's mean energy is a sum over its matrix's columns.
    const int C = numChannels(windows.order), H = head.channels(), L = static_cast<int>(cancel.outputs.size());
    std::vector<double> weight(sz(C));
    for (int c = 0; c < C; ++c) weight[sz(c)] = 1.0 / (2.0 * acnOrder(c) + 1.0);
    const SpeakerDesign dry = windowedDecoder(design, windows);
    double full = 0.0, kept = 0.0;
    for (std::size_t i = 0; i < design.channel.size(); ++i) {
        const double g = i < design.gain.size() ? design.gain[i] : 1.0;
        const std::size_t row = sz(design.channel[i]) * sz(C);
        for (int c = 0; c < C; ++c) {
            full += g * g * design.matrix[row + sz(c)] * design.matrix[row + sz(c)] * weight[sz(c)];
            kept += g * g * dry.matrix[row + sz(c)] * dry.matrix[row + sz(c)] * weight[sz(c)];
        }
    }

    //  The fill: the HRTF window's weighted Gram matrix M = W diag(weight) W^T, mean over the audible bins as the
    //  cancellation's level is.
    int n = 1;
    while (n < cancel.taps + head.taps()) n *= 2;
    RealFft fft;
    if (!fft.prepare(sz(n))) return 1.0;
    const int bins = n / 2 + 1, audible = std::min(bins, static_cast<int>(20000.0 * n / sampleRate) + 1);
    std::vector<double> x(sz(n)), re(sz(bins)), im(sz(bins));
    const auto spectrumOf = [&](std::span<const float> f, std::vector<Cx>& to) {
        std::fill(x.begin(), x.end(), 0.0);
        for (std::size_t t = 0; t < f.size() && t < sz(n); ++t) x[t] = f[t];
        fft.forward(x, re, im);
        for (int k = 0; k < audible; ++k) to[sz(k)] = {re[sz(k)], im[sz(k)]};
    };
    std::vector<Cx> Hs(sz(H) * 2 * sz(audible)), Gs(sz(2) * sz(L) * sz(audible));
    for (int r = 0; r < H; ++r)
        for (int m = 0; m < 2; ++m) {
            std::vector<Cx> s(sz(audible));
            spectrumOf(head.filter(r, m), s);
            std::copy(s.begin(), s.end(), Hs.begin() + static_cast<std::ptrdiff_t>((sz(r) * 2 + sz(m)) * sz(audible)));
        }
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < L; ++i) {
            std::vector<Cx> s(sz(audible));
            const std::size_t at = (sz(m) * sz(L) + sz(i)) * sz(cancel.taps);
            spectrumOf(std::span<const float>(cancel.filters.data() + at, sz(cancel.taps)), s);
            std::copy(s.begin(), s.end(),
                      Gs.begin() + static_cast<std::ptrdiff_t>((sz(m) * sz(L) + sz(i)) * sz(audible)));
        }
    std::vector<double> M(sz(H) * sz(H), 0.0);
    for (int a = 0; a < H; ++a)
        for (int b = 0; b < H; ++b)
            for (int c = 0; c < C; ++c)
                M[sz(a) * sz(H) + sz(b)] +=
                    windows.hrtf[sz(a) * sz(C) + sz(c)] * weight[sz(c)] * windows.hrtf[sz(b) * sz(C) + sz(c)];
    //  Per bin the loudspeakers' feeds from the two ears, P = sum_i G_i G_i^H, against the ears' signals under the
    //  window, Q = Hs^T M Hs*: both 2 x 2, so the loudspeakers and the harmonics are each summed once.
    double filled = 0.0;
    for (int k = 0; k < audible; ++k) {
        Cx P[2][2] = {}, Q[2][2] = {};
        for (int i = 0; i < L; ++i)
            for (int m = 0; m < 2; ++m)
                for (int q = 0; q < 2; ++q)
                    P[m][q] += Gs[(sz(m) * sz(L) + sz(i)) * sz(audible) + sz(k)] *
                               std::conj(Gs[(sz(q) * sz(L) + sz(i)) * sz(audible) + sz(k)]);
        for (int a = 0; a < H; ++a)
            for (int b = 0; b < H; ++b) {
                const double w = M[sz(a) * sz(H) + sz(b)];
                if (w == 0.0) continue;
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q)
                        Q[m][q] += Hs[(sz(a) * 2 + sz(m)) * sz(audible) + sz(k)] * w *
                                   std::conj(Hs[(sz(b) * 2 + sz(q)) * sz(audible) + sz(k)]);
            }
        for (int m = 0; m < 2; ++m)
            for (int q = 0; q < 2; ++q) filled += (P[m][q] * Q[m][q]).real();
    }
    filled /= audible;
    const double played = kept + filled;
    return played > 0.0 ? std::sqrt(full / played) : 1.0;
}

}  // namespace bambi
