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

/// The texel the beams measured most energy in, which is not the first one drawn at full brightness.
int brightestTexel(const EnergyField& field) {
    int best = 0;
    for (int t = 1; t < kEnergyTexels; ++t)
        if (field.energyAt(t, true) > field.energyAt(best, true)) best = t;
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
            const auto at = EnergyField::directionOf(brightestTexel(field));
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
        for (int n = 0; n <= order; ++n) weights[static_cast<std::size_t>(n)] *= 2 * n + 1;

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
            CHECK(std::abs(field.energyAt(texel, true) - wanted) <= 1e-5 * std::max(1.0, wanted));
        }
    }

    SUBCASE("a diffuse field is drawn evenly, at its own level") {
        /*  Catches: a mean or peak taken out of the scale, which would make the same diffuse field vanish or
            come out lumpy -- and a texel's alpha depending on its direction instead of its energy. */
        field.clear();
        std::vector<float> diffuse(static_cast<std::size_t>(covarianceSize(order)), 0.0f);
        const int channels = numChannels(order);
        for (int c = 0; c < channels; ++c) diffuse[static_cast<std::size_t>(covarianceIndex(channels, c, c))] = 1.0f;
        field.sample(diffuse, {});
        field.step(0.0);
        const auto alpha = field.alpha();
        for (const float a : alpha) CHECK(std::abs(a - alpha[0]) < 1e-3);
        CHECK(alpha[0] > 0.2f);  // unit energy in every channel is a loud field, and the same everywhere

        for (float& v : diffuse) v *= 1e-7f;  // 70 dB down
        field.clear();
        field.sample(diffuse, {});
        field.step(0.0);
        for (const float a : field.alpha()) CHECK(a == 0.0f);
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

    SUBCASE("a source looks the same whatever else is playing") {
        /*  Catches: a scale hung from the field's own peak, which draws a lone quiet source at full brightness,
            or from its mean, which dims it when a loud one arrives elsewhere. */
        const Vec3 quietAt{1.0, 0.0, 0.0};
        auto quiet = covarianceOf(planeWave(quietAt, order, 0.03), order);  // -30 dB
        field.clear();
        field.sample(quiet, {});
        field.step(0.0);
        const int texel = brightestTexel(field);
        const float alone = field.alpha()[static_cast<std::size_t>(texel)];
        CHECK(alone > 0.2f);
        CHECK(alone < 0.9f);

        const auto loud = covarianceOf(planeWave({-1.0, 0.0, 0.0}, order, 1.0), order);
        for (std::size_t i = 0; i < quiet.size(); ++i) quiet[i] += loud[i];
        field.clear();
        field.sample(quiet, {});
        field.step(0.0);
        //  the loud source's own tail adds energy here, so the texel can only get brighter, never dimmer
        CHECK(field.alpha()[static_cast<std::size_t>(texel)] >= alone);
    }

    SUBCASE("the brightness is a smooth curve of the level between the dark and the full point") {
        /*  Catches: a wrong dB scaling (amplitude for energy), a hard edge at the dark point, a full point that is
            never reached, or a level not measured against a full-scale point source. */
        const auto alphaAtLevel = [&](double db) {
            field.clear();
            field.sample(covarianceOf(planeWave({1.0, 0.0, 0.0}, order, std::pow(10.0, db / 20.0)), order), {});
            field.step(0.0);
            return field.alpha()[static_cast<std::size_t>(brightestTexel(field))];
        };
        CHECK(alphaAtLevel(0.0) == 1.0f);
        CHECK(alphaAtLevel(kEnergyFullDb) == doctest::Approx(1.0).epsilon(1e-3));
        CHECK(alphaAtLevel(kEnergyDarkDb) < 1e-3);
        CHECK(alphaAtLevel(-60.0) == 0.0f);
        CHECK(std::abs(alphaAtLevel(0.5 * (kEnergyDarkDb + kEnergyFullDb)) - 0.5) < 0.01);
        float last = -1.0f;
        for (double db = -50.0; db <= 0.0; db += 2.5) {
            const float a = alphaAtLevel(db);
            CHECK(a >= last);
            last = a;
        }
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

    SUBCASE("the far side of a plane wave is dim but not dark") {
        field.clear();
        field.sample(covarianceOf(planeWave({1.0, 0.0, 0.0}, order), order), {});
        field.step(0.0);
        CHECK(field.alpha()[static_cast<std::size_t>(brightestTexel(field))] > 0.9f);
        int antipode = 0;
        double closest = -2.0;
        for (int t = 0; t < kEnergyTexels; ++t) {
            const double c = dot(EnergyField::directionOf(t), Vec3{-1.0, 0.0, 0.0});
            if (c > closest) {
                closest = c;
                antipode = t;
            }
        }
        //  a max-rE beam's rear lobe is about 23 dB down at order 3: dim, and well under the peak
        CHECK(field.alpha()[static_cast<std::size_t>(antipode)] < 0.6f);
        CHECK(field.alpha()[static_cast<std::size_t>(antipode)] > 0.3f);
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
            CHECK(std::abs((at.x + 1.0) / 2.0 * kEnergyWidth - (ix + 0.5)) < 1e-9);
            CHECK(std::abs((1.0 - at.y) / 2.0 * kEnergyHeight - (iy + 0.5)) < 1e-9);
            CHECK(std::abs(EnergyField::columnOf(azimuth(d)) - ix) < 1e-9);
        }
    //  and said plainly: a quarter of the way across from the left is the left, which is +y
    CHECK(EnergyField::directionOf((kEnergyHeight / 2) * kEnergyWidth + kEnergyWidth / 4).y > 0.9);
}

}  // namespace bambi
