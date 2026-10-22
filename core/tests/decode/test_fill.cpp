// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "bambi/decode/fill.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

namespace {

using Cx = std::complex<double>;

/// core/data/decode/ku100.json, built as the fill builds it: order 3, its own bulk delay, both sliders at rest.
const BinauralFilterBuilder& ku100(double sampleRate) {
    static BinauralFilterBuilder b48, b96;
    BinauralFilterBuilder& b = sampleRate > 48000.0 ? b96 : b48;
    if (b.channels() == 0) {
        HeadModel m;
        std::string error;
        REQUIRE_MESSAGE(parseHeadModel(readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/ku100.json"), m, &error),
                        error);
        std::vector<Vec3> dirs;
        std::vector<double> w;
        quadratureGrid(12, 20, dirs, w);
        REQUIRE(b.prepare(m, fillFilterDesign(sampleRate), dirs, w));
        b.build(0.0, 1.0);
    }
    return b;
}

Layout layoutOf(const std::vector<std::array<double, 2>>& azEl) {
    Layout l;
    for (std::size_t i = 0; i < azEl.size(); ++i) {
        Loudspeaker s;
        s.azimuthDeg = azEl[i][0];
        s.elevationDeg = azEl[i][1];
        s.radiusM = 2.0;
        s.channel = static_cast<int>(i) + 1;
        l.speakers.push_back(s);
    }
    return l;
}

/// A laptop's two speakers: 34 degrees apart, 30 below the ears.
Layout laptop() { return layoutOf({{16.7, -30.4}, {-17.8, -31.0}}); }

/// The two ears' responses to a plane wave from `from` through the fill's binaural filters, `n` samples.
std::array<std::vector<double>, 2> ears(const BinauralFilterBuilder& head, Vec3 from, int n) {
    std::vector<double> y(static_cast<std::size_t>(numChannels(kFillOrder)));
    shSN3D(from, kFillOrder, y);
    std::array<std::vector<double>, 2> out{std::vector<double>(static_cast<std::size_t>(n)),
                                           std::vector<double>(static_cast<std::size_t>(n))};
    for (int e = 0; e < 2; ++e)
        for (int c = 0; c < head.channels(); ++c) {
            const auto f = head.filter(c, e);
            for (int t = 0; t < head.taps(); ++t)
                out[static_cast<std::size_t>(e)][static_cast<std::size_t>(t)] +=
                    y[static_cast<std::size_t>(c)] * f[static_cast<std::size_t>(t)];
        }
    return out;
}

std::vector<Cx> spectrum(RealFft& fft, const std::vector<double>& x) {
    const std::size_t n = fft.size(), bins = fft.bins();
    std::vector<double> in(n, 0.0), re(bins), im(bins);
    std::copy_n(x.begin(), std::min(n, x.size()), in.begin());
    fft.forward(in, re, im);
    std::vector<Cx> s(bins);
    for (std::size_t k = 0; k < bins; ++k) s[k] = {re[k], im[k]};
    return s;
}

/// One cancellation filter, ear e to row i, as a spectrum.
std::vector<Cx> filterSpectrum(RealFft& fft, const Cancellation& c, int e, int i) {
    const std::size_t L = c.outputs.size(), at = (static_cast<std::size_t>(e) * L + static_cast<std::size_t>(i)) *
                                                 static_cast<std::size_t>(c.taps);
    return spectrum(fft, std::vector<double>(c.filters.begin() + static_cast<std::ptrdiff_t>(at),
                                             c.filters.begin() + static_cast<std::ptrdiff_t>(at) + c.taps));
}

/// The loudspeaker playing on output `channel` (counted from 0), as the layout places it.
Vec3 placeOf(const Layout& l, int channel) {
    for (const Loudspeaker& s : l.speakers)
        if (s.channel == channel + 1) return direction(s.azimuthDeg, s.elevationDeg);
    FAIL("no loudspeaker on that channel");
    return {};
}

/*  What each ear hears of the other ear's channel, mean over the band, dB: the cancellation's filters played from
 *  where the layout puts each output's loudspeaker, through the same head. Worked out here from the layout itself,
 *  not from the design, so a row sent to the wrong output shows. */
double separationAtEars(const Layout& l, const Cancellation& c, const BinauralFilterBuilder& head, double sampleRate,
                        const FillSettings& s) {
    RealFft fft;
    REQUIRE(fft.prepare(4096));
    const int L = static_cast<int>(c.outputs.size());
    std::vector<std::array<std::vector<Cx>, 2>> H, G;  // per row: ears from it; filters into it from each ear
    for (int i = 0; i < L; ++i) {
        const auto e = ears(head, placeOf(l, c.outputs[static_cast<std::size_t>(i)]), 4096);
        H.push_back({spectrum(fft, e[0]), spectrum(fft, e[1])});
        G.push_back({filterSpectrum(fft, c, 0, i), filterSpectrum(fft, c, 1, i)});
    }
    double ratio = 0.0;
    int n = 0;
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        const double hz = static_cast<double>(k) * sampleRate / 4096.0;
        if (hz < s.bandLoHz || hz > s.bandHiHz) continue;
        Cx E[2][2] = {};
        for (int l2 = 0; l2 < 2; ++l2)
            for (int m = 0; m < 2; ++m)
                for (int i = 0; i < L; ++i)
                    E[l2][m] += H[static_cast<std::size_t>(i)][static_cast<std::size_t>(l2)][k] *
                                G[static_cast<std::size_t>(i)][static_cast<std::size_t>(m)][k];
        ratio += (std::norm(E[0][1]) + std::norm(E[1][0])) / (std::norm(E[0][0]) + std::norm(E[1][1]));
        ++n;
    }
    return 10.0 * std::log10(ratio / n);
}

/// The dry share a plane wave from `v` keeps under the windows, at order 3: both windows evaluated where it comes from.
double dryShare(const FillWindows& w, Vec3 v) {
    REQUIRE(w.order == kFillOrder);
    const int C = numChannels(kFillOrder);
    std::vector<double> y(static_cast<std::size_t>(C));
    shSN3D(v, kFillOrder, y);
    double d = 0.0, h = 0.0, yy = 0.0;
    for (int r = 0; r < C; ++r) {
        for (int c = 0; c < C; ++c) {
            d += y[static_cast<std::size_t>(r)] * w.dry[static_cast<std::size_t>(r * C + c)] *
                 y[static_cast<std::size_t>(c)];
            h += y[static_cast<std::size_t>(r)] * w.hrtf[static_cast<std::size_t>(r * C + c)] *
                 y[static_cast<std::size_t>(c)];
        }
        yy += y[static_cast<std::size_t>(r)] * y[static_cast<std::size_t>(r)];
    }
    d = std::max(0.0, d / yy);
    h = std::max(0.0, h / yy);
    return d * d / (d * d + h * h);
}

}  // namespace

TEST_CASE("through the HRTF and the cancellation, a loudspeaker's own direction plays that loudspeaker alone") {
    //  Catches: the ears or the loudspeakers swapped, a missing conjugate in the inversion, a delay the dry part
    //  would not match, times not kept at another rate.
    for (const double rate : {48000.0, 96000.0}) {
        CAPTURE(rate);
        const Layout l = laptop();
        const SpeakerDesign d = design(l, 3);
        FillSettings s;
        s.regularisationDb = -30.0;  // deep enough to ask 0.5 dB of it; the default is tuned by ear
        Cancellation c;
        std::string error;
        REQUIRE_MESSAGE(designCancellation(d, ku100(rate), s, rate, c, &error), error);
        CHECK(c.delay == static_cast<int>(std::lround(128 * rate / 48000.0)));
        CHECK(c.taps == static_cast<int>(std::lround(384 * rate / 48000.0)));

        RealFft fft;
        REQUIRE(fft.prepare(8192));
        for (int from = 0; from < 2; ++from) {
            CAPTURE(from);
            const auto e = ears(ku100(rate), placeOf(l, from), 8192);
            const std::array<std::vector<Cx>, 2> B{spectrum(fft, e[0]), spectrum(fft, e[1])};
            std::array<std::vector<Cx>, 2> X{std::vector<Cx>(fft.bins()), std::vector<Cx>(fft.bins())};
            for (int i = 0; i < 2; ++i)
                for (int m = 0; m < 2; ++m) {
                    const std::vector<Cx> g = filterSpectrum(fft, c, m, i);
                    for (std::size_t k = 0; k < fft.bins(); ++k)
                        X[static_cast<std::size_t>(c.outputs[static_cast<std::size_t>(i)])][k] +=
                            g[k] * B[static_cast<std::size_t>(m)][k];
                }
            double worstDb = 0.0, own = 0.0, other = 0.0, slope = 0.0;
            int slopes = 0;
            for (std::size_t k = 1; k < fft.bins(); ++k) {
                const double hz = static_cast<double>(k) * rate / 8192.0;
                if (hz < s.bandLoHz || hz > s.bandHiHz) continue;
                const Cx self = X[static_cast<std::size_t>(from)][k];
                worstDb = std::max(worstDb, std::abs(10.0 * std::log10(std::norm(self))));
                own += std::norm(self);
                other += std::norm(X[static_cast<std::size_t>(1 - from)][k]);
                if (hz > 600.0 && hz < 0.7 * s.bandHiHz) {
                    slope +=
                        std::remainder(std::arg(self) - std::arg(X[static_cast<std::size_t>(from)][k - 1]), 2.0 * kPi);
                    ++slopes;
                }
            }
            const double delay = -slope / slopes * 8192.0 / (2.0 * kPi);
            CHECK(worstDb < 0.5);
            CHECK(std::abs(delay - c.delay) < 0.5);
            CHECK(10.0 * std::log10(other / own) < -30.0);
        }
    }
}

TEST_CASE("the cancellation gives each ear its own channel on layouts that are neither a pair nor symmetric") {
    //  Catches: a loudspeaker's filters sent to another loudspeaker's output, a cancellation solved for a symmetric
    //  pair only.
    const FillSettings s;
    const Layout irregular = layoutOf({{25, -5}, {-40, 10}, {100, 0}, {-150, 20}, {60, 50}});
    for (const Layout* l : {bs2051Layout("0+5+0"), &irregular}) {
        Cancellation c;
        std::string error;
        REQUIRE_MESSAGE(designCancellation(design(*l, 3), ku100(48000.0), s, 48000.0, c, &error), error);
        CHECK(c.outputs.size() == 5);
        const double sep = separationAtEars(*l, c, ku100(48000.0), 48000.0, s);
        CHECK(sep < -30.0);
        CHECK(std::abs(sep - c.separationDb) < 3.0);
    }
}

TEST_CASE("with equal amplitude the two windows sum to the identity") {
    //  Catches: the wrong per-degree SN3D scale in the projection, a quadrature too coarse for the order.
    const CoveredRegion region = coveredRegion(design(laptop(), 3));
    FillSettings s;
    s.equalPower = false;
    for (const int order : {1, 3, 5, 7}) {
        CAPTURE(order);
        FillWindows w;
        fillWindows(region, s, order, w);
        const int C = numChannels(order), H = numChannels(kFillOrder);
        double worst = 0.0;
        for (int r = 0; r < std::min(C, H); ++r)
            for (int c = 0; c < C; ++c)
                worst = std::max(worst, std::abs(w.dry[static_cast<std::size_t>(r * C + c)] +
                                                 w.hrtf[static_cast<std::size_t>(r * C + c)] - (r == c ? 1.0 : 0.0)));
        CHECK(worst < 1e-9);
    }
    //  Every row at order 7: a layout covering the sphere keeps all of it dry, so its dry window is the identity.
    const CoveredRegion all =
        coveredRegion(design(layoutOf({{0, 0}, {90, 0}, {180, 0}, {-90, 0}, {0, 90}, {0, -90}}), 3));
    FillWindows w;
    fillWindows(all, s, 7, w);
    const int C = numChannels(7);
    double worst = 0.0;
    for (int r = 0; r < C; ++r)
        for (int c = 0; c < C; ++c)
            worst = std::max(worst, std::abs(w.dry[static_cast<std::size_t>(r * C + c)] - (r == c ? 1.0 : 0.0)));
    CHECK(worst < 1e-9);
}

TEST_CASE("at order 3 a source between a pair's loudspeakers stays dry, and one behind goes binaural") {
    //  Catches: a dry region that lost its fully-dry zone (62 % dry there without it), a window applied the wrong
    //  way round.
    const SpeakerDesign d = design(laptop(), 3);
    FillWindows w;
    fillWindows(coveredRegion(d), FillSettings{}, kFillOrder, w);
    const Vec3 between = unit(d.points[0] + d.points[1]);
    CHECK(dryShare(w, between) > 0.99);
    CHECK(dryShare(w, direction(180, 0)) < 0.01);
}

TEST_CASE("a layout covering the sphere is left alone, and one that cannot cancel is refused") {
    //  Catches: a fill that runs where it cannot help, a refusal that never comes.
    const SpeakerDesign octahedron = design(layoutOf({{0, 0}, {90, 0}, {180, 0}, {-90, 0}, {0, 90}, {0, -90}}), 3);
    const CoveredRegion all = coveredRegion(octahedron);
    CHECK(all.sphere);
    FillWindows w;
    fillWindows(all, FillSettings{}, 3, w);
    double worst = 0.0;
    for (std::size_t k = 0; k < w.hrtf.size(); ++k) worst = std::max(worst, std::abs(w.hrtf[k]));
    CHECK(worst < 1e-12);
    const SpeakerDesign same = windowedDecoder(octahedron, w);
    for (std::size_t k = 0; k < same.matrix.size(); ++k) CHECK(std::abs(same.matrix[k] - octahedron.matrix[k]) < 1e-9);

    //  Both on the median plane: they reach the two ears almost alike.
    Cancellation c;
    std::string error;
    CHECK_FALSE(
        designCancellation(design(layoutOf({{0, 0}, {0, 90}}), 3), ku100(48000.0), FillSettings{}, 48000.0, c, &error));
    CHECK(c.separationDb > kFillSeparationNeededDb);
    CHECK_FALSE(error.empty());
}

TEST_CASE("what a layout covers: a dome's coplanar square, a ring's arcs, not its imaginary loudspeakers") {
    //  Catches: the centre of 7.1.4's height square taken for a gap, the pole a dome lacks taken as covered, the
    //  covered triangles' inner edges taken for its border, an imaginary loudspeaker on a ring taken as covered.
    const CoveredRegion dome = coveredRegion(design(*bs2051Layout("4+7+0"), 3));
    CHECK_FALSE(dome.sphere);
    CHECK(angleToRegion(dome, direction(0, 90)) == doctest::Approx(0.0));
    CHECK(angleToRegion(dome, direction(0, -90)) == doctest::Approx(0.5 * kPi));
    CHECK(angleToRegion(dome, direction(15, -30)) == doctest::Approx(30.0 * kDeg2Rad));  // to the edge, not a corner

    const CoveredRegion five = coveredRegion(design(*bs2051Layout("0+5+0"), 3));
    CHECK(angleToRegion(five, direction(180, 0)) < 1e-9);
    CHECK(angleToRegion(five, direction(0, 40)) == doctest::Approx(40.0 * kDeg2Rad));

    Layout open = layoutOf({{30, 0}, {-30, 0}, {110, 0}, {-110, 0}, {180, 0}});
    open.speakers.back().imaginary = true;
    const CoveredRegion ring = coveredRegion(design(open, 3));
    CHECK(angleToRegion(ring, direction(180, 0)) == doctest::Approx(70.0 * kDeg2Rad));
    CHECK(angleToRegion(ring, direction(70, 0)) < 1e-9);
}

TEST_CASE("outside the band each ear's channel plays on the one loudspeaker nearest 30 degrees to its side") {
    //  Catches: the uncancelled feed spread over every loudspeaker on a side, a pair chosen by channel order or by
    //  side alone.
    const FillSettings s;
    //  The channels in an order where the first loudspeaker on each side is not the one that should play.
    const Layout irregular = layoutOf({{100, 0}, {-150, 20}, {60, 50}, {25, -5}, {-40, 10}});
    struct Want {
        const Layout* layout;
        double left, right;  // the azimuths that should carry the left and the right ear
    };
    for (const Want& w :
         {Want{bs2051Layout("0+5+0"), 30, -30}, Want{bs2051Layout("4+7+0"), 30, -30}, Want{&irregular, 25, -40}}) {
        Cancellation c;
        std::string error;
        REQUIRE_MESSAGE(designCancellation(design(*w.layout, 3), ku100(48000.0), s, 48000.0, c, &error), error);
        RealFft fft;
        REQUIRE(fft.prepare(4096));
        for (int e = 0; e < 2; ++e) {
            std::vector<double> energy;
            double total = 0.0;
            for (std::size_t i = 0; i < c.outputs.size(); ++i) {
                const std::vector<Cx> g = filterSpectrum(fft, c, e, static_cast<int>(i));
                double sum = 0.0;
                for (std::size_t k = 0; k < fft.bins(); ++k) {
                    const double hz = static_cast<double>(k) * 48000.0 / 4096.0;
                    if (hz > 1.4 * s.bandHiHz && hz < 18000.0) sum += std::norm(g[k]);
                }
                energy.push_back(sum);
                total += sum;
            }
            const std::size_t loudest =
                static_cast<std::size_t>(std::max_element(energy.begin(), energy.end()) - energy.begin());
            const Vec3 at = placeOf(*w.layout, c.outputs[loudest]);
            CHECK(azimuth(at) * kRad2Deg == doctest::Approx(e == 0 ? w.left : w.right));
            CHECK(10.0 * std::log10((total - energy[loudest]) / energy[loudest]) < -40.0);
        }
    }
}

TEST_CASE("the cancellation's feeds carry each loudspeaker's distance gain and delay") {
    //  Catches: the feeds left aligned while the renderer delays and scales the dry part of a room that is not a
    //  sphere, a delay applied to the wrong loudspeaker.
    Layout near = laptop(), far = laptop();
    near.speakers[0].radiusM = near.speakers[1].radiusM = 2.0;
    far.speakers[0].radiusM = 2.0;
    far.speakers[1].radiusM = 1.0;  // the right one a metre nearer: delayed and quietened to meet the left
    const SpeakerDesign a = design(near, 3), b = design(far, 3);
    Cancellation ca, cb;
    REQUIRE(designCancellation(a, ku100(48000.0), FillSettings{}, 48000.0, ca));
    REQUIRE(designCancellation(b, ku100(48000.0), FillSettings{}, 48000.0, cb));
    const int late = static_cast<int>(std::lround(b.delaySeconds[1] * 48000.0));
    REQUIRE(late > 100);
    CHECK(cb.taps == ca.taps + late);
    for (int e = 0; e < 2; ++e)
        for (int i = 0; i < 2; ++i) {
            const float g = static_cast<float>(b.gain[static_cast<std::size_t>(i)]);
            const int shift = i == 1 ? late : 0;
            double worst = 0.0, peak = 0.0;
            for (int t = 0; t < ca.taps; ++t) {
                const float want = g * ca.filters[static_cast<std::size_t>((e * 2 + i) * ca.taps + t)];
                const float got = cb.filters[static_cast<std::size_t>((e * 2 + i) * cb.taps + t + shift)];
                worst = std::max(worst, static_cast<double>(std::abs(got - want)));
                peak = std::max(peak, static_cast<double>(std::abs(want)));
            }
            CHECK(worst <= 1e-6 * peak);
        }
}
