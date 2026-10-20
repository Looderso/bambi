// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bambi/decode/allrad.hpp"
#include "bambi/decode/layoutfile.hpp"
#include "bambi/decode/layouts.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/patch/json.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

/*  The array on hand (array16.json, an IEM AllRADecoder file: 17 loudspeakers on channels 1-3 and 5-18, the file's
 *  own order-5 N3D matrix) and what an independent design in numpy made of it
 *  (array16-reference.json: orders 1-3, max-rE, SN3D, energy 1, channel 18 imaginary). The file marks channel 18,
 *  straight below, as a real loudspeaker; the room has none there, so the design treats it as imaginary. */
namespace {
Loudspeaker at(double az, double el, int channel, double r = 1.0, bool imaginary = false) {
    Loudspeaker s;
    s.azimuthDeg = az;
    s.elevationDeg = el;
    s.channel = channel;
    s.radiusM = r;
    s.imaginary = imaginary;
    return s;
}

/// ITU-R BS.2051 4+7+0: seven at ear level, four above, nothing below; channel 4 is the LFE and absent here.
Layout itu470() {
    Layout l;
    l.speakers = {at(0, 0, 1),    at(30, 0, 2),  at(-30, 0, 3),   at(90, 0, 5),    at(-90, 0, 6),   at(135, 0, 7),
                  at(-135, 0, 8), at(45, 30, 9), at(-45, 30, 10), at(135, 30, 11), at(-135, 30, 12)};
    return l;
}

/// ITU-R BS.2051 0+2+0: the pair at plus and minus 30 degrees.
Layout pair() {
    Layout l;
    l.speakers = {at(30, 0, 1), at(-30, 0, 2)};
    return l;
}
/// The loudspeaker gains a plane wave from t gets.
std::vector<double> gainsAt(const SpeakerDesign& d, Vec3 t) {
    const int C = numChannels(d.order);
    std::vector<double> y(static_cast<std::size_t>(C)), g(static_cast<std::size_t>(d.outputs), 0.0);
    shSN3D(t, d.order, y);
    for (int o = 0; o < d.outputs; ++o)
        for (int c = 0; c < C; ++c)
            g[static_cast<std::size_t>(o)] +=
                d.matrix[static_cast<std::size_t>(o * C + c)] * y[static_cast<std::size_t>(c)];
    return g;
}

double meanEnergy(const SpeakerDesign& d, const std::vector<Vec3>& grid) {
    double e = 0;
    for (const Vec3& t : grid)
        for (double g : gainsAt(d, t)) e += g * g;
    return e / static_cast<double>(grid.size());
}

/// DECODER 3.2's reading of a design: energy range (dB), mean rE and median direction error, above and below -30 degrees.
struct Reading {
    double energyMin, energyMax, rE, direction;
};
std::array<Reading, 2> read(const SpeakerDesign& d) {
    std::vector<Vec3> grid;
    fibonacciSphere(4000, grid);
    std::vector<Vec3> speaker(static_cast<std::size_t>(d.outputs));
    for (std::size_t s = 0, p = 0; p < d.points.size(); ++p) {
        if (d.imaginary[p]) continue;
        speaker[static_cast<std::size_t>(d.channel[s++])] = d.points[p];
    }
    std::array<std::vector<double>, 2> energy, re, angle;
    for (const Vec3& t : grid) {
        const std::vector<double> g = gainsAt(d, t);
        double e = 0;
        Vec3 rv{0, 0, 0};
        for (int o = 0; o < d.outputs; ++o) {
            const double p = g[static_cast<std::size_t>(o)] * g[static_cast<std::size_t>(o)];
            e += p;
            rv = rv + speaker[static_cast<std::size_t>(o)] * p;
        }
        rv = rv * (1.0 / e);
        const double len = std::sqrt(rv.x * rv.x + rv.y * rv.y + rv.z * rv.z);
        const std::size_t side = t.z > -0.5 ? 0 : 1;
        energy[side].push_back(10.0 * std::log10(e));
        re[side].push_back(len);
        angle[side].push_back(std::acos(std::clamp((rv.x * t.x + rv.y * t.y + rv.z * t.z) / len, -1.0, 1.0)) *
                              kRad2Deg);
    }
    std::array<Reading, 2> out{};
    for (std::size_t side = 0; side < 2; ++side) {
        std::sort(angle[side].begin(), angle[side].end());
        double sum = 0;
        for (double v : re[side]) sum += v;
        out[side] = {*std::min_element(energy[side].begin(), energy[side].end()),
                     *std::max_element(energy[side].begin(), energy[side].end()),
                     sum / static_cast<double>(re[side].size()), angle[side][angle[side].size() / 2]};
    }
    return out;
}
}  // namespace

TEST_CASE("the design matches the IEM plug-in's own matrix for the array") {
    //  Catches: a wrong hull, wrong VBAP gains, a wrong spherical-harmonic convention.
    Layout l;
    std::string error;
    REQUIRE(parseIemLayout(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16.json"), l, &error));
    REQUIRE(l.speakers.size() == 17);  // channel 18 real, as the file has it
    const Json& decoder = *arrayFile().find("Decoder");
    const std::vector<Json>& ref = decoder.find("Matrix")->items();
    const std::vector<Json>& routing = decoder.find("Routing")->items();
    const int order = static_cast<int>(std::lround(std::sqrt(static_cast<double>(ref[0].size())))) - 1;
    REQUIRE(order == 5);
    const SpeakerDesign d = design(l, order, Weighting::Basic);
    const int C = numChannels(order);
    // Ours is for SN3D in and energy-normalised; the file's for N3D, at its own level: scale per column, then fit one gain.
    double dotProduct = 0, ours2 = 0, ref2 = 0;
    std::vector<double> mine(ref.size() * static_cast<std::size_t>(C));
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const int row = static_cast<int>(routing[i].numberOr(0)) - 1;
        for (int c = 0; c < C; ++c) {
            const double v = d.matrix[static_cast<std::size_t>(row * C + c)] / std::sqrt(2.0 * acnOrder(c) + 1.0);
            const double r = ref[i].items()[static_cast<std::size_t>(c)].numberOr(0);
            mine[i * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] = v;
            dotProduct += v * r;
            ours2 += v * v;
            ref2 += r * r;
        }
    }
    const double scale = dotProduct / ours2;
    double err2 = 0;
    for (std::size_t i = 0; i < ref.size(); ++i)
        for (int c = 0; c < C; ++c) {
            const double diff = scale * mine[i * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] -
                                ref[i].items()[static_cast<std::size_t>(c)].numberOr(0);
            err2 += diff * diff;
        }
    CHECK(std::sqrt(err2 / ref2) < 3e-4);  // the independent design: 0.02 %
}

TEST_CASE("the design equals an independent one, max-rE at orders 1 to 3") {
    //  Catches: a missing SN3D scaling, wrong max-rE weights, a wrong or missing energy normalisation.
    const Json ref = Json::parse(readAll(std::string(BAMBI_CORE_DIR) + "/tests/decode/array16-reference.json"));
    const Layout l = array16();
    for (int order : {1, 2, 3}) {
        const SpeakerDesign d = design(l, order, Weighting::MaxRe);
        const int C = numChannels(order);
        REQUIRE(d.outputs == 17);
        const std::vector<Json>& rows = ref.find(("order" + std::to_string(order)).c_str())->items();
        REQUIRE(rows.size() == 16);
        double worst = 0;
        std::size_t i = 0;
        for (const Loudspeaker& s : l.speakers) {
            if (s.imaginary) continue;
            for (int c = 0; c < C; ++c)
                worst = std::max(worst, std::abs(d.matrix[static_cast<std::size_t>((s.channel - 1) * C + c)] -
                                                 rows[i].items()[static_cast<std::size_t>(c)].numberOr(0)));
            ++i;
        }
        CHECK_MESSAGE(worst < 1e-5, "order " << order << ": " << worst);
    }
}

TEST_CASE("the array reads as DECODER 3.2 measured it") {
    //  Catches: the imaginary point dropped from the hull instead of from the output, which would even out the bottom.
    const Layout l = array16();
    const double energy[3][4] = {{-0.8, 0.9, -1.5, 0.6}, {-0.9, 1.7, -5.6, 1.0}, {-1.0, 2.0, -9.7, 1.1}};
    const double rE[3] = {0.64, 0.80, 0.86}, above[3] = {7.1, 3.8, 3.9}, below[3] = {52, 49, 49};
    for (int order : {1, 2, 3}) {
        const std::array<Reading, 2> r = read(design(l, order, Weighting::MaxRe));
        const std::size_t o = static_cast<std::size_t>(order - 1);
        CHECK(std::abs(r[0].energyMin - energy[o][0]) < 0.1);
        CHECK(std::abs(r[0].energyMax - energy[o][1]) < 0.1);
        CHECK(std::abs(r[1].energyMin - energy[o][2]) < 0.1);
        CHECK(std::abs(r[1].energyMax - energy[o][3]) < 0.1);
        CHECK(std::abs(r[0].rE - rE[o]) < 0.01);
        CHECK(std::abs(r[0].direction - above[o]) < 0.3);
        CHECK(std::abs(r[1].direction - below[o]) < 1.0);
    }
}

TEST_CASE("an imaginary loudspeaker has no output and is not mixed back") {
    //  Catches: the imaginary point's gains folded into its neighbours, which would make the bottom as loud as the top.
    const SpeakerDesign d = design(array16(), 3, Weighting::MaxRe);
    CHECK(d.outputs == 17);  // channel 18 has no row
    CHECK(d.channel.size() == 16);
    const std::vector<double> down = gainsAt(d, {0, 0, -1}), front = gainsAt(d, {1, 0, 0});
    double eDown = 0, eFront = 0;
    for (double g : down) eDown += g * g;
    for (double g : front) eFront += g * g;
    CHECK(eDown / eFront < 0.5);
    for (int order : {1, 2, 3})
        CHECK(design(array16(), order, Weighting::Basic).points.size() == 17);  // nothing added: the file has the nadir
}

TEST_CASE("every decoder is energy-preserving, whatever the weighting or the layout") {
    //  Catches: a missing normalisation, or one that depends on the weighting.
    std::vector<Vec3> same, other;
    fibonacciSphere(4000, same);
    fibonacciSphere(3000, other);
    for (const Layout& l : {array16(), itu470(), pair()})
        for (Weighting w : {Weighting::Basic, Weighting::MaxRe}) {
            const SpeakerDesign d = design(l, 3, w);
            CHECK(meanEnergy(d, same) == doctest::Approx(1.0).epsilon(1e-9));
            CHECK(meanEnergy(d, other) == doctest::Approx(1.0).epsilon(0.01));
        }
}

TEST_CASE("the same layout designs the same decoder, however it is listed") {
    //  Catches: tie-breaking in the hull that depends on the input order -- 7.1.4 has coplanar groups.
    const Layout ordered = itu470();
    const SpeakerDesign a = design(ordered, 3, Weighting::Basic), again = design(ordered, 3, Weighting::Basic);
    CHECK(a.matrix == again.matrix);
    CHECK(a.triangles == again.triangles);
    CHECK(a.points.size() == 14);  // eleven loudspeakers, the point below, the centres of the height square and
                                   // of the back wall (M and U at +-135, a coplanar four)
    Layout shuffled = ordered;
    std::shuffle(shuffled.speakers.begin(), shuffled.speakers.end(), std::mt19937(5));
    REQUIRE(shuffled.speakers.front().channel != ordered.speakers.front().channel);
    const SpeakerDesign b = design(shuffled, 3, Weighting::Basic);
    CHECK(a.matrix == b.matrix);
    CHECK(a.triangles == b.triangles);
    CHECK(a.channel == b.channel);
}

TEST_CASE("a ring is designed on the ring") {
    //  Catches: a ring put through the 3D hull, where it would get two poles and pan into them.
    Layout l;
    for (int i = 0; i < 8; ++i) l.speakers.push_back(at(45.0 * i, 0, i + 1));
    const SpeakerDesign d = design(l, 3, Weighting::Basic);
    CHECK(d.ring);
    CHECK(d.triangles.empty());
    CHECK(d.points.size() == 8);
    std::vector<Vec3> ring;
    for (int k = 0; k < 720; ++k) ring.push_back({std::cos(kPi * k / 360.0), std::sin(kPi * k / 360.0), 0.0});
    CHECK(meanEnergy(d, ring) == doctest::Approx(1.0).epsilon(1e-6));
    // Close to the sampling decoder on the same ring: each loudspeaker's row the harmonics at its direction.
    const int C = numChannels(3);
    std::vector<double> sampling(static_cast<std::size_t>(8 * C)), y(static_cast<std::size_t>(C));
    for (int s = 0; s < 8; ++s) {
        shSN3D(d.points[static_cast<std::size_t>(s)], 3, y);
        for (int c = 0; c < C; ++c)
            sampling[static_cast<std::size_t>(s * C + c)] = y[static_cast<std::size_t>(c)] * (2.0 * acnOrder(c) + 1.0);
    }
    SpeakerDesign s = d;
    s.matrix = sampling;
    const double level = std::sqrt(meanEnergy(s, ring));
    double diff2 = 0, ref2 = 0;
    for (std::size_t i = 0; i < sampling.size(); ++i) {
        const double v = sampling[i] / level;
        diff2 += (d.matrix[i] - v) * (d.matrix[i] - v);
        ref2 += v * v;
    }
    // Measured: 20 % apart at order 3, the panning sharper than the sampling (rE 0.88 against 0.84); a sphere design
    // with two poles would be far off and would have triangles.
    CHECK(std::sqrt(diff2 / ref2) < 0.25);
}

TEST_CASE("a layout that does not surround the listener is refused, with a reason") {
    //  Catches: a silent failure that designs a decoder with the listener outside the hull.
    Layout front;
    int ch = 1;
    for (double az : {-60.0, -30.0, 0.0, 30.0, 60.0})
        for (double el : {0.0, 30.0}) front.speakers.push_back(at(az, el, ch++));
    SpeakerDesign d;
    std::string error;
    CHECK_FALSE(designAllrad(front, 3, Weighting::Basic, d, &error));
    CHECK(error.find("surround") != std::string::npos);

    Layout twice = itu470();
    twice.speakers[1].channel = twice.speakers[0].channel;
    CHECK_FALSE(designAllrad(twice, 1, Weighting::Basic, d, &error));
    CHECK_FALSE(error.empty());

    Layout none;
    none.speakers = {at(0, 0, 1, 1, true), at(120, 0, 2, 1, true), at(-120, 0, 3, 1, true), at(0, 90, 4, 1, true)};
    CHECK_FALSE(designAllrad(none, 1, Weighting::Basic, d, &error));
}

TEST_CASE("distance: the farthest loudspeaker plays at one with no delay, the others follow r / r_max") {
    //  Catches: an inverted ratio, or the delay on the far loudspeaker.
    const SpeakerDesign d = design(array16(), 1, Weighting::Basic);
    REQUIRE(d.gain.size() == 16);
    CHECK(d.channel[0] == 0);
    CHECK(d.gain[0] == doctest::Approx(1.0));  // channel 1, 1.65 m, the farthest
    CHECK(d.delaySeconds[0] == doctest::Approx(0.0));
    CHECK(d.channel[5] == 6);  // channel 7 at 1.39 m
    CHECK(d.gain[5] == doctest::Approx(1.39 / 1.65).epsilon(1e-6));
    CHECK(d.delaySeconds[5] == doctest::Approx((1.65 - 1.39) / 343.0).epsilon(1e-6));
}

TEST_CASE("a coplanar group of loudspeakers pans evenly: 7.1.4 is left-right symmetric") {
    //  Catches: the hull's arbitrary diagonal through the height square, which favours one pair of loudspeakers.
    const SpeakerDesign d = design(*bs2051Layout("4+7+0"), 3, Weighting::Basic);  // channels in the ITU order
    REQUIRE(d.points.size() == 14);
    CHECK(d.imaginary[12]);
    CHECK(d.imaginary[13]);
    CHECK(d.points[12].z == doctest::Approx(1.0));  // the height square's centre, straight up
    CHECK(d.points[13].x < -0.9);                   // the back wall's centre, behind, a little up
    std::vector<Vec3> grid;
    fibonacciSphere(500, grid);
    const int C = numChannels(3);
    double worst = 0;
    for (const Vec3& t : grid) {
        const std::vector<double> g = gainsAt(d, t), m = gainsAt(d, {t.x, -t.y, t.z});
        // Mirrored source, mirrored loudspeakers: channels (1,2), (5,6), (7,8), (9,10), (11,12) swap; 3 stays.
        const int pair[12] = {1, 0, 2, 3, 5, 4, 7, 6, 9, 8, 11, 10};
        for (int o = 0; o < 12; ++o)
            worst = std::max(worst, std::abs(g[static_cast<std::size_t>(o)] - m[static_cast<std::size_t>(pair[o])]));
    }
    CHECK(worst <
          0.01);  // the design grid itself is not mirror-symmetric, which leaves about 1e-3; a diagonal, 0.1 and more
    (void)C;
}

TEST_CASE("two loudspeakers are a pair of virtual cardioids a quarter turn to each side: nothing vanishes") {
    //  Catches: a pair put through the ring design, which would drop everything off the ring; a pattern with a
    //  rear lobe of the other sign; the microphones pointed at the loudspeakers, which images only two to three;
    //  a tilted pair's microphones laid on the horizon instead of in the pair's plane.
    const SpeakerDesign d = design(pair(), 3, Weighting::Basic);
    REQUIRE(d.outputs == 2);
    const auto front = gainsAt(d, fromAzEl(0, 0)), left = gainsAt(d, fromAzEl(30 * kDeg2Rad, 0)),
               side = gainsAt(d, fromAzEl(kPi / 2, 0)), behind = gainsAt(d, fromAzEl(kPi, 0)),
               above = gainsAt(d, fromAzEl(0, kPi / 2));
    CHECK(front[0] == doctest::Approx(front[1]).epsilon(1e-9));
    CHECK(left[0] == doctest::Approx(3.0 * left[1]).epsilon(1e-6));   // 1 + cos 60 against 1 + cos 120
    CHECK(side[0] == doctest::Approx(2.0 * front[0]).epsilon(1e-6));  // the left microphone's axis
    CHECK(side[1] < 1e-9);
    CHECK(behind[0] == doctest::Approx(front[0]).epsilon(1e-9));  // behind folds in at the front's level
    CHECK(behind[1] == doctest::Approx(front[1]).epsilon(1e-9));
    CHECK(above[0] == doctest::Approx(above[1]).epsilon(1e-9));
    //  a tilted pair points its microphones in its own plane, not along the horizon
    Layout tilted;
    tilted.speakers = {at(45, 45, 1), at(-45, -45, 2)};
    const SpeakerDesign r = design(tilted, 3, Weighting::Basic);
    const Vec3 p1 = fromAzEl(45 * kDeg2Rad, 45 * kDeg2Rad), p2 = fromAzEl(-45 * kDeg2Rad, -45 * kDeg2Rad);
    const Vec3 b = unit(p1 + p2), axisLeft = unit(p1 - b * dot(p1, b));
    CHECK(gainsAt(r, axisLeft)[0] == doctest::Approx(2.0 * gainsAt(r, b)[0]).epsilon(1e-6));
    CHECK(gainsAt(r, axisLeft)[1] < 1e-9);
    CHECK(gainsAt(r, Vec3{0, 1, 0})[0] < 0.95 * gainsAt(r, axisLeft)[0]);
    //  a plane wave from anywhere gives the omni's share at least: no direction is silent
    std::vector<Vec3> grid;
    fibonacciSphere(500, grid);
    for (const Vec3& t : grid) {
        const auto g = gainsAt(d, t);
        CHECK(g[0] + g[1] > 0.05);
    }
    //  one loudspeaker is an omni: every direction the same
    Layout one;
    one.speakers = {at(0, 0, 1)};
    const SpeakerDesign mono = design(one, 3, Weighting::MaxRe);
    REQUIRE(mono.outputs == 1);
    CHECK(gainsAt(mono, fromAzEl(0, 0))[0] == doctest::Approx(gainsAt(mono, fromAzEl(2.0, -0.7))[0]).epsilon(1e-9));
    CHECK(gainsAt(mono, fromAzEl(0, 0))[0] == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("a narrow pair at the loudspeakers hears the slice between them and little else") {
    //  Catches: a pattern power expanded with the wrong Legendre coefficients (a rear lobe, or a pattern no
    //  narrower than the cardioid), the microphones left at the sides when asked for the loudspeakers, the power
    //  not held to the field's order.
    Microphones narrow;
    narrow.atLoudspeakers = true;
    narrow.pattern = 3;
    SpeakerDesign d;
    std::string error;
    REQUIRE(designAllrad(pair(), 3, Weighting::Basic, d, &error, narrow));
    const auto left = gainsAt(d, fromAzEl(30 * kDeg2Rad, 0)), front = gainsAt(d, fromAzEl(0, 0)),
               side = gainsAt(d, fromAzEl(kPi / 2, 0)), behind = gainsAt(d, fromAzEl(kPi, 0));
    CHECK(left[0] > 2.2 * left[1]);  // on the left loudspeaker's axis: 1 against (1 + cos 60)^3 / 8 = 0.42
    CHECK(front[0] == doctest::Approx(front[1]).epsilon(1e-9));
    CHECK(20 * std::log10(side[0] / left[0]) < -7.0);     // a third of a turn off the axis
    CHECK(20 * std::log10(behind[0] / left[0]) < -30.0);  // behind: gone, and not of the other sign
    CHECK(behind[0] >= 0.0);
    //  the cardioid to the first power is the cardioid of before: the same design
    Microphones wide;
    wide.pattern = 1;
    SpeakerDesign w1, w0;
    REQUIRE(designAllrad(pair(), 3, Weighting::Basic, w1, &error, wide));
    REQUIRE(designAllrad(pair(), 3, Weighting::Basic, w0, &error));
    for (std::size_t i = 0; i < w1.matrix.size(); ++i)
        CHECK(w1.matrix[i] == doctest::Approx(w0.matrix[i]).epsilon(1e-9));
    //  a power past the field's order is held to it: at order 1 the narrow pair is the cardioid pair
    SpeakerDesign held, first;
    REQUIRE(designAllrad(pair(), 1, Weighting::Basic, held, &error, narrow));
    Microphones one = narrow;
    one.pattern = 1;
    REQUIRE(designAllrad(pair(), 1, Weighting::Basic, first, &error, one));
    for (std::size_t i = 0; i < held.matrix.size(); ++i)
        CHECK(held.matrix[i] == doctest::Approx(first.matrix[i]).epsilon(1e-9));
    //  and it is energy-preserving like every decoder
    std::vector<Vec3> grid;
    fibonacciSphere(4000, grid);
    CHECK(meanEnergy(d, grid) == doctest::Approx(1.0).epsilon(1e-9));
}
