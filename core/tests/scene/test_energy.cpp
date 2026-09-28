// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <vector>

#include "../doctest.h"
#include "bambi/math/legendre.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/scene/energy.hpp"
#include "bambi/scene/view.hpp"

namespace bambi {

namespace {

/// A plane wave from `from`, as a field: one frame, SN3D encoded.
std::vector<float> planeWave(Vec3 from, int order, double gain = 1.0) {
    std::vector<double> y(static_cast<std::size_t>(numChannels(order)), 0.0);
    shSN3D(unit(from), order, y);
    std::vector<float> out(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) out[i] = static_cast<float>(y[i] * gain);
    return out;
}

/// The covariance of one repeated frame, as CovarianceWindow would publish it.
std::vector<float> covarianceOf(const std::vector<float>& frame, int order) {
    CovarianceWindow w;
    w.prepare(order, 1);
    w.add(frame.data(), 1);
    std::vector<float> out(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
    w.take(out);
    return out;
}

int brightestTexel(std::span<const float> alpha) {
    int best = 0;
    for (int t = 1; t < kEnergyTexels; ++t)
        if (alpha[static_cast<std::size_t>(t)] > alpha[static_cast<std::size_t>(best)]) best = t;
    return best;
}

}  // namespace

TEST_CASE("a covariance window summarises a field without drawing it") {
    constexpr int order = 3;
    const auto frame = planeWave({1.0, 0.0, 0.0}, order);

    SUBCASE("the packing is the upper triangle, and it is the MEAN over the window") {
        //  Catches: dividing by the wrong count, which would scale every energy by how long a window was.
        CovarianceWindow w;
        w.prepare(order, 4);
        CHECK(!w.ready());
        for (int i = 0; i < 4; ++i) w.add(frame.data(), 1);
        CHECK(w.ready());

        std::vector<float> got(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
        w.take(got);
        //  four identical frames average to one of them: C_00 = y_0 * y_0
        CHECK(got[0] == doctest::Approx(frame[0] * frame[0]).epsilon(1e-6));
        CHECK(!w.ready());  // taking starts the next window
    }

    SUBCASE("a reset forgets a part-finished window") {
        //  `frame` is one frame, so it is added one at a time: asking add() for four of them would
        //  read three frames past the end of it.
        CovarianceWindow w;
        w.prepare(order, 8);
        for (int i = 0; i < 4; ++i) w.add(frame.data(), 1);
        w.reset();
        CHECK(!w.ready());
        for (int i = 0; i < 8; ++i) w.add(frame.data(), 1);
        std::vector<float> got(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
        w.take(got);
        CHECK(got[0] == doctest::Approx(frame[0] * frame[0]).epsilon(1e-6));
    }

    SUBCASE("the index maths addresses every pair exactly once") {
        const int channels = numChannels(order);
        std::vector<int> seen(static_cast<std::size_t>(covarianceSize(order)), 0);
        for (int i = 0; i < channels; ++i)
            for (int j = i; j < channels; ++j) ++seen[static_cast<std::size_t>(covarianceIndex(channels, i, j))];
        for (const int n : seen) CHECK(n == 1);
    }
}

TEST_CASE("the energy field points at the energy") {
    constexpr int order = 3;
    EnergyField field;
    field.prepare(order);

    SUBCASE("a plane wave is brightest in its own direction") {
        /*  Catches: the max-rE weights left out, the world pole used instead of the texel's
            direction, or the off-diagonal terms' factor of two dropped -- each moves or flattens this. */
        for (const Vec3 from : {Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, unit({0.3, -0.5, 0.8})}) {
            field.clear();
            const auto cov = covarianceOf(planeWave(from, order), order);
            field.sample(cov, {});
            field.step(1.0);  // a long step, so the one-pole has settled
            const auto at = EnergyField::directionOf(brightestTexel(field.alpha()));
            //  within a texel's own width: 128 x 64 is under three degrees a texel
            CHECK(dot(at, unit(from)) > std::cos(6.0 * kDeg2Rad));
        }
    }

    SUBCASE("the energy is the whole quadratic form, not half of it") {
        /*  An independent oracle: the test builds the full symmetric covariance and evaluates
            b^T C b over both triangles, where the code walks the packed upper one and counts the
            off-diagonal terms twice. Dropping that factor leaves the picture peaking in exactly the
            same direction -- every check that only asks where is blind to it. */
        field.clear();
        const auto frame = planeWave(unit({0.4, 0.7, -0.3}), order, 1.7);
        const auto packed = covarianceOf(frame, order);
        field.sample(packed, {});
        field.step(1.0);

        const int channels = numChannels(order);
        std::vector<double> weights(static_cast<std::size_t>(order + 1), 1.0);
        maxRE(order, weights);

        for (const int texel : {0, 137, 2048, kEnergyTexels - 1}) {
            std::vector<double> y(static_cast<std::size_t>(channels), 0.0);
            shSN3D(EnergyField::directionOf(texel), order, y);
            double wanted = 0.0;
            for (int i = 0; i < channels; ++i)
                for (int j = 0; j < channels; ++j) {
                    const double c = static_cast<double>(
                        packed[static_cast<std::size_t>(covarianceIndex(channels, std::min(i, j), std::max(i, j)))]);
                    wanted += y[static_cast<std::size_t>(i)] * weights[static_cast<std::size_t>(acnOrder(i))] *
                              y[static_cast<std::size_t>(j)] * weights[static_cast<std::size_t>(acnOrder(j))] * c;
                }
            /*  1e-5, not 1e-9: the beam table and the sum are float, because at order 7 the table
                is the difference between a picture that costs a quarter of a core and one that
                costs half. The oracle is double, so they agree to float's precision. */
            CHECK(field.energyAt(texel, true) == doctest::Approx(wanted).epsilon(1e-5));
        }
    }

    SUBCASE("a diffuse field disappears, because the mean is the zero of the scale") {
        /*  Catches: the mean not subtracted, which lights this up evenly instead of vanishing --
            the whole of what the SHAPE ballistic is for. */
        field.clear();
        std::vector<float> diffuse(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
        const int channels = numChannels(order);
        for (int c = 0; c < channels; ++c) diffuse[static_cast<std::size_t>(covarianceIndex(channels, c, c))] = 1.0f;
        field.sample(diffuse, {});
        field.step(1.0);
        float brightest = 0.0f;
        for (const float a : field.alpha()) brightest = std::max(brightest, a);
        CHECK(brightest < 0.05f);
        //  and it is the floor that does it, not luck: nothing is hung from a peak this small
        CHECK(field.reference() == 0.0);
    }

    SUBCASE("the colour says which energy is whose") {
        //  Catches: wet and dry swapped, or the share read off the total instead -- either inverts this.
        field.clear();
        const auto front = covarianceOf(planeWave({1.0, 0.0, 0.0}, order), order);
        const auto back = covarianceOf(planeWave({-1.0, 0.0, 0.0}, order), order);
        field.sample(front, back);
        field.step(1.0);

        const auto shareAt = [&](Vec3 d) {
            int best = 0;
            double bestDot = -2.0;
            for (int t = 0; t < kEnergyTexels; ++t) {
                const double c = dot(EnergyField::directionOf(t), unit(d));
                if (c > bestDot) {
                    bestDot = c;
                    best = t;
                }
            }
            return field.addedOnly()[static_cast<std::size_t>(best)];
        };
        CHECK(shareAt({1.0, 0.0, 0.0}) == 1.0f);   // the front is what this plugin added
        CHECK(shareAt({-1.0, 0.0, 0.0}) == 0.0f);  // the back is what arrived

        /*  Where both are, the input wins, even under a louder output, and nothing is between.
            Catches: the colour taken by majority, which reads the front as the plugin's; or by
            share, which reads it as neither. */
        field.clear();
        auto quieter = front;
        for (auto& v : quieter) v *= 0.25f;  // 6 dB under what the plugin adds there
        field.sample(front, quieter);
        field.step(1.0);
        CHECK(shareAt({1.0, 0.0, 0.0}) == 0.0f);
        for (const float v : field.addedOnly()) CHECK((v == 0.0f || v == 1.0f));
    }

    SUBCASE("the reference jumps to a peak at once and falls at 10 dB a second") {
        //  Catches: a fall at the wrong rate, or one applied in amplitude rather than energy.
        field.clear();
        const auto cov = covarianceOf(planeWave({1.0, 0.0, 0.0}, order), order);
        field.sample(cov, {});
        for (int i = 0; i < 50; ++i) field.step(0.01);  // 0.5 s: the one-pole has settled
        const double peak = field.reference();
        CHECK(peak > 0.0);

        /*  The reference falls only once the energy has gone -- it is the peak or the fall,
            whichever is higher, and a peak that is still there is still the peak. So the field is
            given silence, and then timed over half a second.

            Every step stays inside the 1.6 s hold: past it, `step` stops painting and returns before
            the fall is applied at all. */
        std::vector<float> silence(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
        field.sample(silence, {});
        for (int i = 0; i < 50; ++i) field.step(0.01);  // the energy decays away
        const double quiet = field.reference();
        CHECK(quiet > 0.0);
        for (int i = 0; i < 50; ++i) field.step(0.01);  // and half a second of fall
        CHECK(10.0 * std::log10(field.reference() / quiet) == doctest::Approx(-5.0).epsilon(1e-3));

        //  and it jumps back up: a new peak is taken at once, never approached
        field.sample(cov, {});
        for (int i = 0; i < 50; ++i) field.step(0.01);
        CHECK(field.reference() == doctest::Approx(peak).epsilon(1e-3));
    }

    SUBCASE("painting stops 1.6 s after the last covariance") {
        field.clear();
        field.sample(covarianceOf(planeWave({1.0, 0.0, 0.0}, order), order), {});
        CHECK(field.step(0.05));
        CHECK(field.step(1.0));
        CHECK(field.step(0.5));
        CHECK(!field.step(0.2));  // past 1.6
        float brightest = 0.0f;
        for (const float a : field.alpha()) brightest = std::max(brightest, a);
        CHECK(brightest == 0.0f);
    }

    SUBCASE("nothing is drawn more than 30 dB under the reference") {
        field.clear();
        const auto cov = covarianceOf(planeWave({1.0, 0.0, 0.0}, order), order);
        field.sample(cov, {});
        field.step(1.0);
        //  The peak is at the reference, so it is drawn fully; the antipode of a plane wave is far
        //  under it at order 3, so it is not drawn at all.
        CHECK(field.alpha()[static_cast<std::size_t>(brightestTexel(field.alpha()))] > 0.9f);
        int antipode = 0;
        double closest = -2.0;
        for (int t = 0; t < kEnergyTexels; ++t) {
            const double c = dot(EnergyField::directionOf(t), Vec3{-1.0, 0.0, 0.0});
            if (c > closest) {
                closest = c;
                antipode = t;
            }
        }
        CHECK(field.alpha()[static_cast<std::size_t>(antipode)] == 0.0f);
    }
}

/*  Catches the texture's columns running the other way from the equirect view's: the energy would
 *  be drawn mirrored on the equirect, left for right, and right on the globe, which resamples by
 *  direction and so would not notice. */
TEST_CASE("the energy texture runs the way the equirect view draws it: left is left") {
    for (const int ix : {0, 17, kEnergyWidth / 4, kEnergyWidth / 2, kEnergyWidth - 1})
        for (const int iy : {3, kEnergyHeight / 2, kEnergyHeight - 5}) {
            const Vec3 d = EnergyField::directionOf(iy * kEnergyWidth + ix);
            const auto at = project(Projection::Equirect, Camera{}, d);
            //  where the view puts that direction, as a fraction across and down, is where the texel is
            CHECK((at.x + 1.0) / 2.0 * kEnergyWidth == doctest::Approx(ix + 0.5).epsilon(1e-9));
            CHECK((1.0 - at.y) / 2.0 * kEnergyHeight == doctest::Approx(iy + 0.5).epsilon(1e-9));
            CHECK(EnergyField::columnOf(azimuth(d)) == doctest::Approx(static_cast<double>(ix)).epsilon(1e-9));
        }
    //  and said plainly: a quarter of the way across from the left is the left, which is +y
    CHECK(EnergyField::directionOf((kEnergyHeight / 2) * kEnergyWidth + kEnergyWidth / 4).y > 0.9);
}

}  // namespace bambi
