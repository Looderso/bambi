// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <string>
#include <vector>

#include "bambi/decode/decoder.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/measuredhead.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/sphere.hpp"
#include "doctest.h"
#include "fixtures.hpp"

using namespace bambi;
using namespace bambi::test;

/*  A measured head is fitted into the model's tables and must come back as it went in. The head here is made, not
 *  measured: the two ears' levels are smooth functions of the direction with a known difference and a known time
 *  difference, built as linear-phase responses, so every table has a truth to be checked against.
 */
namespace {
constexpr double kRate = 48000.0;
constexpr int kTaps = 2048;

//  left louder on the left (y > 0), both brighter above (z > 0), the left ear earlier on the left
double leftDb(const Vec3& v, double hz) {
    return -3.0 * (1.0 - v.y) * hz / (hz + 2000.0) + 2.0 * v.z * hz / (hz + 6000.0);
}
double rightDb(const Vec3& v, double hz) {
    return -3.0 * (1.0 + v.y) * hz / (hz + 2000.0) + 2.0 * v.z * hz / (hz + 6000.0);
}
double itdUs(const Vec3& v) { return -600.0 * v.y; }

MeasuredHead madeHead(const std::vector<Vec3>& directions) {
    MeasuredHead m;
    m.sampleRate = kRate;
    m.taps = kTaps;
    m.directions = directions;
    m.name = "a made head";
    m.left.resize(directions.size() * kTaps);
    m.right.resize(directions.size() * kTaps);
    RealFft fft;
    REQUIRE(fft.prepare(kTaps));
    const int bins = kTaps / 2 + 1;
    std::vector<double> re(bins), im(bins), ir(kTaps);
    const double base = 0.002;  // seconds: both ears' responses sit 2 ms in
    for (std::size_t d = 0; d < directions.size(); ++d) {
        const Vec3& v = directions[d];
        for (int ear = 0; ear < 2; ++ear) {
            const double delay = base + (ear == 0 ? 0.5 : -0.5) * itdUs(v) * 1e-6;
            for (int k = 0; k < bins; ++k) {
                const double hz = k * kRate / kTaps;
                const double a = std::pow(10.0, (ear == 0 ? leftDb(v, hz) : rightDb(v, hz)) / 20.0);
                const double ph = -2.0 * kPi * hz * delay;
                re[static_cast<std::size_t>(k)] = a * std::cos(ph);
                im[static_cast<std::size_t>(k)] = a * std::sin(ph);
            }
            fft.inverse(re, im, ir);
            float* out = (ear == 0 ? m.left : m.right).data() + d * kTaps;
            for (int i = 0; i < kTaps; ++i) out[i] = static_cast<float>(ir[static_cast<std::size_t>(i)]);
        }
    }
    return m;
}

double cllOf(const Vec3& v, double hz) {
    return 10.0 * std::log10(std::pow(10.0, leftDb(v, hz) / 10.0) + std::pow(10.0, rightDb(v, hz) / 10.0));
}
double erbToHz(double erb) { return (std::pow(10.0, erb / 21.4) - 1.0) / 0.00437; }

/// RMS over 1-16 kHz of the model against the truth at `directions`: summed level, difference, time difference.
HeadFitError againstTruth(const HeadModel& model, const std::vector<Vec3>& directions) {
    Head head;
    head.prepare(model, directions);
    head.synthesise(0.0, 1.0);
    HeadFitError e;
    double sc = 0, si = 0, st = 0;
    std::size_t cells = 0;
    for (std::size_t d = 0; d < directions.size(); ++d) {
        const int di = static_cast<int>(d);
        for (int k = 0; k < head.points(); ++k) {
            const double hz = erbToHz(head.erbs()[static_cast<std::size_t>(k)]);
            if (hz < 1000.0 || hz >= 16000.0) continue;
            const double l = head.left(di)[static_cast<std::size_t>(k)],
                         r = head.right(di)[static_cast<std::size_t>(k)];
            const double cll = 10.0 * std::log10(std::pow(10.0, l / 10.0) + std::pow(10.0, r / 10.0));
            const double dc = cll - cllOf(directions[d], hz);
            const double dl = (l - r) - (leftDb(directions[d], hz) - rightDb(directions[d], hz));
            sc += dc * dc;
            si += dl * dl;
            ++cells;
        }
        const double dt = head.itd(di) - itdUs(directions[d]);
        st += dt * dt;
    }
    e.cllDb = std::sqrt(sc / static_cast<double>(cells));
    e.ildDb = std::sqrt(si / static_cast<double>(cells));
    e.itdUs = std::sqrt(st / static_cast<double>(directions.size()));
    return e;
}
}  // namespace

TEST_CASE("the time difference is measured with its sign: the ear a sound reaches first is the earlier one") {
    //  Catches: the ears swapped in the cross-correlation, the lag read from the wrong end of the window, or an
    //  estimate a whole sample coarse.
    const MeasuredHead m = madeHead({direction(90.0, 0.0), direction(-30.0, 10.0), direction(0.0, 0.0)});
    ItdEstimator itd;
    REQUIRE(itd.prepare(kRate));
    const auto at = [&](std::size_t d) {
        return itd.microseconds(std::span<const float>(m.left.data() + d * kTaps, kTaps),
                                std::span<const float>(m.right.data() + d * kTaps, kTaps));
    };
    CHECK(at(0) == doctest::Approx(-600.0).epsilon(0.01));  // on the left: left first, negative
    CHECK(at(1) == doctest::Approx(600.0 * std::cos(10.0 * kDeg2Rad) * 0.5).epsilon(0.01));  // 30 right, 10 up
    CHECK(std::abs(at(2)) < 2.0);                                                            // in front: none
}

TEST_CASE("a made head comes back from its fit: levels, difference and time difference, and a file round trip") {
    //  Catches: the difference fitted with the ears swapped, the levels read from the wrong bin or scaled twice,
    //  the time difference fitted at the wrong order's prefix, a writer that drops a table or rounds it away.
    std::vector<Vec3> grid;
    fibonacciSphere(1200, grid);
    const MeasuredHead m = madeHead(grid);
    HeadFit fit;
    fit.order = 12;
    fit.itdOrder = 6;
    HeadModel model;
    std::string error;
    REQUIRE_MESSAGE(fitHeadModel(m, fit, model, &error), error);
    CHECK(model.learnedFrom == "a made head");
    CHECK(model.notches.list.empty());
    CHECK(model.smooth.order == 12);
    //  against the set itself, as the tool reports it
    const HeadFitError self = headFitError(m, model, 1000.0, 16000.0);
    CHECK(self.cllDb < 0.05);
    CHECK(self.ildDb < 0.05);
    CHECK(self.itdUs < 4.0);
    //  and against the truth at directions the fit never saw
    std::vector<Vec3> unseen;
    fibonacciSphere(500, unseen);
    const HeadFitError truth = againstTruth(model, unseen);
    CHECK(truth.cllDb < 0.05);
    CHECK(truth.ildDb < 0.05);
    CHECK(truth.itdUs < 4.0);
    //  the left ear is the louder one on the left, after the whole chain
    Head head;
    head.prepare(model, std::vector<Vec3>{direction(90.0, 0.0)});
    head.synthesise(0.0, 1.0);
    const std::size_t top = static_cast<std::size_t>(head.points() - 1);
    CHECK(head.left(0)[top] > head.right(0)[top] + 4.0);
    CHECK(head.itd(0) < -500.0);

    const std::string json = headModelJson(model);
    HeadModel back;
    REQUIRE_MESSAGE(parseHeadModel(json, back, &error), error);
    CHECK(back.learnedFrom == model.learnedFrom);
    CHECK(back.smooth.coef.size() == model.smooth.coef.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < model.smooth.coef.size(); ++i)
        worst = std::max(worst, std::abs(back.smooth.coef[i] - model.smooth.coef[i]));
    for (std::size_t i = 0; i < model.ild.coef.size(); ++i)
        worst = std::max(worst, std::abs(back.ild.coef[i] - model.ild.coef[i]));
    CHECK(worst < 0.0006);
    for (std::size_t i = 0; i < model.itd.coef.size(); ++i)
        CHECK(std::abs(back.itd.coef[i] - model.itd.coef[i]) < 0.06);
}

TEST_CASE("a grid with no points below the listener fits without running wild there") {
    //  Catches: the roughness penalty dropped, or applied without the order's weight: the unconstrained high orders
    //  then swing by tens of dB in the gap.
    std::vector<Vec3> whole;
    fibonacciSphere(1500, whole);
    std::vector<Vec3> grid, gap;
    for (const Vec3& v : whole) (v.z < -0.6 ? gap : grid).push_back(v);  // nothing below -37 degrees
    const MeasuredHead m = madeHead(grid);
    HeadFit fit;
    fit.order = 16;
    fit.itdOrder = 8;
    HeadModel model;
    std::string error;
    REQUIRE_MESSAGE(fitHeadModel(m, fit, model, &error), error);
    const HeadFitError inside = againstTruth(model, grid), below = againstTruth(model, gap);
    CHECK(inside.cllDb < 0.1);
    CHECK(below.cllDb < 1.0);
    CHECK(below.ildDb < 1.5);
    CHECK(below.itdUs < 40.0);
}

TEST_CASE("the decoder takes another head between builds, and its filters change") {
    //  Catches: setModel that leaves the synthesised head where it was, or a model change that the next work()
    //  does not build.
    std::vector<Vec3> grid;
    fibonacciSphere(600, grid);
    const MeasuredHead m = madeHead(grid);
    HeadFit fit;
    fit.order = 8;
    fit.itdOrder = 4;
    HeadModel made;
    REQUIRE(fitHeadModel(m, fit, made));
    BinauralDecoder decoder;
    FilterDesign design;
    design.order = 1;
    REQUIRE(decoder.prepare(shipped(), design, 64, 8, 12));
    REQUIRE(decoder.work());
    const std::vector<float> first(decoder.filters().filter(0, 0).begin(), decoder.filters().filter(0, 0).end());
    CHECK_FALSE(decoder.stale());
    decoder.setModel(made);
    CHECK(decoder.stale());
    REQUIRE(decoder.work());
    const std::span<const float> second = decoder.filters().filter(0, 0);
    double diff = 0;
    for (std::size_t i = 0; i < first.size(); ++i) diff += std::abs(first[i] - second[i]);
    CHECK(diff > 1e-3);
}

TEST_CASE("the shipped KU100 is the measured head at order 20, and nothing else") {
    //  Catches: a stale or wrong file in core/data/decode, a model with a notch layer passed off as a measurement.
    HeadModel m;
    std::string error;
    REQUIRE_MESSAGE(parseHeadModel(readAll(std::string(BAMBI_CORE_DIR) + "/data/decode/ku100.json"), m, &error), error);
    CHECK(m.learnedFrom.find("KU100") != std::string::npos);
    CHECK(m.smooth.order == 20);
    CHECK(m.ild.order == 20);
    CHECK(m.itd.order == 10);
    CHECK(m.axis.step == doctest::Approx(0.2));
    CHECK(m.notches.list.empty());
    bool sizeMorph = false;
    for (double v : m.ildWithSize.coef) sizeMorph = sizeMorph || v != 0.0;
    CHECK_FALSE(sizeMorph);
    //  a sound on the left reaches the left ear first, by the KU100's 700-900 us at 90 degrees: a large head
    Head head;
    head.prepare(m, std::vector<Vec3>{direction(90.0, 0.0)});
    head.synthesise(0.0, 1.0);
    CHECK(head.itd(0) < -650.0);
    CHECK(head.itd(0) > -900.0);
}
