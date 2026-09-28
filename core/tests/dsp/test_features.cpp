// SPDX-License-Identifier: GPL-3.0-or-later
//
//  These are the properties the feature bank's calibration rests on -- separability against a
//  real library is measured by bambi-features, but a library run does not say which property
//  broke.

#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "bambi/dsp/features.hpp"
#include "bambi/dsp/fft.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

constexpr double kSr = 48000.0;

std::vector<float> sine(double hz, double seconds, float amp = 0.5f, double sr = kSr) {
    std::vector<float> x(static_cast<std::size_t>(seconds * sr));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
    return x;
}

std::vector<float> noise(double seconds, float amp = 0.5f, double sr = kSr) {
    std::vector<float> x(static_cast<std::size_t>(seconds * sr));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(-amp, amp);
    for (auto& v : x) v = d(rng);
    return x;
}

/// Mean of one feature over gated frames -- the same summary bambi-features reports.
double meanGated(const std::vector<float>& x, Feature f, double sr = kSr) {
    const auto rows = FeatureBank::analyseOffline(x, sr);
    double acc = 0.0;
    int n = 0;
    for (const auto& r : rows) {
        if (r.back() <= 0.5) continue;
        acc += r[static_cast<std::size_t>(f)];
        ++n;
    }
    return n > 0 ? acc / n : 0.0;
}

}  // namespace

TEST_CASE("silence produces no features and never opens the gate") {
    std::vector<float> quiet(static_cast<std::size_t>(kSr), 0.0f);
    const auto rows = FeatureBank::analyseOffline(quiet, kSr);
    REQUIRE(rows.size() > 8);
    for (const auto& r : rows) {
        CHECK(r.back() == doctest::Approx(0.0));  // gate
        for (int f = 0; f < kNumFeatures; ++f) CHECK(r[static_cast<std::size_t>(f)] == doctest::Approx(0.0));
    }
}

/*  The orthogonality the feature set was chosen for. If these two ever converge, the set
 *  has lost the property that made it worth six features instead of one. */
TEST_CASE("Tonal separates a held note from noise, and Attack does not fire on either") {
    const double tonalSine = meanGated(sine(440.0, 2.0), Feature::Tonal);
    const double tonalNoise = meanGated(noise(2.0), Feature::Tonal);
    INFO("sine ", tonalSine, "  noise ", tonalNoise);
    CHECK(tonalSine > 0.6);
    CHECK(tonalNoise < 0.15);

    /*  Neither is transient, so both must read low Attack whatever their spectrum. Not
     *  zero, though: white noise has a genuinely fluctuating short-term envelope, so its
     *  fast/slow ratio sits a little above unity forever. It measures ~0.10 here against
     *  ~0.30 for an actual transient, and that gap is the property being asserted -- a
     *  threshold tight enough to call noise "exactly zero" would be testing a fiction. */
    CHECK(meanGated(sine(440.0, 2.0), Feature::Attack) < 0.1);
    CHECK(meanGated(noise(2.0), Feature::Attack) < 0.15);
}

TEST_CASE("Attack fires on transients and is level-independent") {
    auto clicks = [](float amp) {
        std::vector<float> x(static_cast<std::size_t>(kSr * 2.0), 0.0f);
        for (std::size_t start = 0; start + 400 < x.size(); start += 9600) {  // 5 Hz
            for (std::size_t i = 0; i < 400; ++i)
                x[start + i] = amp * static_cast<float>(std::exp(-static_cast<double>(i) / 60.0) *
                                                        std::sin(0.7 * static_cast<double>(i)));
        }
        return x;
    };
    const double loud = meanGated(clicks(0.8f), Feature::Attack);
    const double quiet = meanGated(clicks(0.1f), Feature::Attack);
    INFO("loud ", loud, "  quiet ", quiet);
    CHECK(loud > 0.25);
    //  Attack is a fast/slow ratio, so an 18 dB level change must barely move it. This is
    //  the property that distinguishes it from Level, and the reason spectral flux was
    //  rejected in its favour.
    CHECK(std::abs(loud - quiet) < 0.15);
}

TEST_CASE("Attack opens continuously, so one ULP cannot move a whole hop of gain") {
    /*  Catches: `slow_ > attackGateFloor` as a branch, which would step Attack from 0 to its
     *  full swing in one hop right at the transient, with gate_ already ~0.58 -- so a one-ULP
     *  difference in slow_ (which a different architecture, or a differently chosen FMA, can
     *  produce) would move 0.58 of gain, and modulation does not smooth a rate destination, so
     *  the motion clock would integrate it into a permanent offset. The property that rules this
     *  out is continuity in the input, not a smaller step: Attack must be a Lipschitz function of
     *  level, like Level itself.
     *
     *  Read at the first analysed frame on purpose: a few hops later the ratio is saturated and
     *  gate_ has reached 1, so broken and fixed both read 1.0 and any mean over frames hides the
     *  step entirely. */
    FeatureBank fb;
    fb.prepare(kSr);
    fb.setSpectralEnabled(false);  // a time-domain property

    auto attackAt = [&](float amp) {
        fb.reset();
        const auto x = sine(300.0, 256.0 / kSr, amp);
        fb.process(x.data(), static_cast<int>(x.size()));
        return fb.value(Feature::Attack);
    };

    std::vector<double> swept;
    for (double amp = 1e-3; amp <= 1e-1; amp *= 1.0116)  // 0.1 dB steps
        swept.push_back(attackAt(static_cast<float>(amp)));

    double worstStep = 0.0;
    for (std::size_t i = 1; i < swept.size(); ++i) worstStep = std::max(worstStep, std::abs(swept[i] - swept[i - 1]));

    INFO("worst adjacent step ", worstStep, " over ", swept.size(), " steps of 0.1 dB");
    //  The branch measures 0.556 here, and so does a hysteresis latch at any separation --
    //  which is why this case pins a ramp rather than a smaller step. The ramp is bounded by its
    //  own width: 0.0116 of a 2.2-factor band, times gate_.
    CHECK(worstStep < 0.05);
    //  Catches: `return 0.0` would otherwise pass the continuity check above.
    CHECK(swept.front() == doctest::Approx(0.0));
    CHECK(swept.back() > 0.5);
}

TEST_CASE("the bands follow the frequency they are named after") {
    const double lowInLow = meanGated(sine(80.0, 1.5), Feature::Low);
    const double lowInHigh = meanGated(sine(80.0, 1.5), Feature::High);
    CHECK(lowInLow > lowInHigh);

    const double hiInHigh = meanGated(sine(6000.0, 1.5), Feature::High);
    const double hiInLow = meanGated(sine(6000.0, 1.5), Feature::Low);
    CHECK(hiInHigh > hiInLow);

    const double midInMid = meanGated(sine(900.0, 1.5), Feature::Mid);
    CHECK(midInMid > meanGated(sine(900.0, 1.5), Feature::Low));
    CHECK(midInMid > meanGated(sine(900.0, 1.5), Feature::High));
}

TEST_CASE("Level rises with amplitude") {
    const double a = meanGated(sine(440.0, 1.0, 0.05f), Feature::Level);
    const double b = meanGated(sine(440.0, 1.0, 0.5f), Feature::Level);
    INFO("quiet ", a, "  loud ", b);
    CHECK(b > a + 0.1);
}

/*  The regression test for the block-based rewrite: process() copies in runs and indexes the
 *  ring with a mask, and a source fed 128 samples at a time must produce bit-identical features
 *  to the same source fed 777 samples at a time, or the hop alignment is wrong and every
 *  calibrated constant is read at the wrong moment. 777 is deliberately not a divisor of the hop. */
TEST_CASE("results do not depend on the block size the host happens to use") {
    const auto x = sine(300.0, 1.0, 0.4f);
    auto runWithBlocks = [&](int block) {
        FeatureBank fb;
        fb.prepare(kSr);
        std::vector<std::array<double, kNumFeatures>> out;
        std::size_t i = 0;
        while (i < x.size()) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - i));
            fb.process(x.data() + i, n);
            std::array<double, kNumFeatures> v{};
            for (int f = 0; f < kNumFeatures; ++f) v[static_cast<std::size_t>(f)] = fb.value(static_cast<Feature>(f));
            out.push_back(v);
            i += static_cast<std::size_t>(n);
        }
        return out.back();  // final state after identical audio
    };
    const auto a = runWithBlocks(128);
    const auto b = runWithBlocks(777);
    const auto c = runWithBlocks(1);
    for (int f = 0; f < kNumFeatures; ++f) {
        INFO("feature ", f);
        CHECK(a[static_cast<std::size_t>(f)] == doctest::Approx(b[static_cast<std::size_t>(f)]).epsilon(1e-12));
        CHECK(a[static_cast<std::size_t>(f)] == doctest::Approx(c[static_cast<std::size_t>(f)]).epsilon(1e-12));
    }
}

TEST_CASE("the gate opens over a band, so a level rounded differently cannot move all of it") {
    /*  Catches: `db > gateDb` as a branch, so a level on the threshold that two machines round
     *  differently moves the gate -- and everything it multiplies -- by all of it. A steady tone
     *  is swept through the threshold in 0.1 dB steps, each held until the gate has settled. */
    FeatureBank fb;
    fb.prepare(kSr);
    fb.setSpectralEnabled(false);
    const auto gateAt = [&](double rmsDb) {
        fb.reset();
        const auto amp = static_cast<float>(std::sqrt(2.0) * std::pow(10.0, rmsDb / 20.0));
        const auto x = sine(300.0, 0.3, amp);
        fb.process(x.data(), static_cast<int>(x.size()));
        return fb.gate();
    };
    const FeatureConfig cfg;
    std::vector<double> swept;
    for (double db = cfg.gateDb - 5.0; db <= cfg.gateDb + 5.0; db += 0.1) swept.push_back(gateAt(db));
    double worstStep = 0.0;
    for (std::size_t i = 1; i < swept.size(); ++i) worstStep = std::max(worstStep, std::abs(swept[i] - swept[i - 1]));
    INFO("worst adjacent step ", worstStep);
    //  The branch steps by 1 here. The ramp is bounded by its width: 0.1 of 3 dB, times the gate's 1.4.
    CHECK(worstStep < 0.06);
    //  Catches: `return 0` would pass the check above; the gate still closes below the band and
    //  opens above it.
    CHECK(swept.front() == doctest::Approx(0.0));
    CHECK(swept.back() == doctest::Approx(1.0));
}

TEST_CASE("reset returns the bank to its initial state") {
    FeatureBank fb;
    fb.prepare(kSr);
    const auto loud = sine(440.0, 0.5);
    fb.process(loud.data(), static_cast<int>(loud.size()));
    REQUIRE(fb.gate() > 0.5);
    fb.reset();
    CHECK(fb.gate() == doctest::Approx(0.0));
    for (int f = 0; f < kNumFeatures; ++f) CHECK(fb.value(static_cast<Feature>(f)) == doctest::Approx(0.0));
}

TEST_CASE("disabling the spectral path zeroes only the features that need the FFT") {
    FeatureBank fb;
    fb.prepare(kSr);
    fb.setSpectralEnabled(false);
    const auto x = sine(440.0, 0.5);
    fb.process(x.data(), static_cast<int>(x.size()));
    CHECK(fb.value(Feature::Tonal) == doctest::Approx(0.0));
    CHECK(fb.value(Feature::Low) == doctest::Approx(0.0));
    CHECK(fb.value(Feature::High) == doctest::Approx(0.0));
    CHECK(fb.value(Feature::Level) > 0.0);  // time-domain features keep working
    CHECK(fb.gate() > 0.5);
}

/*  The two numerical shortcuts the spectral path takes, each checked against the honest
 *  form it replaced. Both are invisible from the feature values, which is exactly why they
 *  need their own tests -- a mistake here would show up as a slow drift in calibration
 *  rather than as anything obviously broken. */
TEST_CASE("power() agrees with magnitude() squared") {
    RealFft fft;
    fft.prepare(1024);
    std::vector<double> in(1024), mag(fft.bins()), pw(fft.bins());
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(0.11 * static_cast<double>(i)) + 0.3 * std::cos(0.9 * static_cast<double>(i));
    fft.magnitude(in, mag);
    fft.power(in, pw);
    for (std::size_t k = 0; k < fft.bins(); ++k) {
        INFO("bin ", k);
        CHECK(pw[k] == doctest::Approx(mag[k] * mag[k]).epsilon(1e-9));
    }
}

TEST_CASE("log of a running product equals the per-bin sum of logs") {
    //  The identity the flatness loop relies on, over a deliberately hostile spread of
    //  magnitudes: 1e-10 to 1e3, which underflows a naive product long before the end.
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> ex(-10.0, 3.0);
    std::vector<double> v(1000);
    for (auto& p : v) p = std::pow(10.0, ex(rng));

    double sumOfLogs = 0.0;
    for (double p : v) sumOfLogs += std::log(p);

    double prod = 1.0;
    int prodExp = 0;
    for (double p : v) {
        prod *= p;
        if (prod < 1e-100) {
            int e = 0;
            prod = std::frexp(prod, &e);
            prodExp += e;
        }
    }
    const double logOfProduct = std::log(prod) + prodExp * 0.6931471805599453;
    INFO("sum ", sumOfLogs, "  product ", logOfProduct);
    CHECK(logOfProduct == doctest::Approx(sumOfLogs).epsilon(1e-12));
}

// ---- a field's detector --------------------------------------------------------------------------

namespace {

/// A mono signal encoded at `from`, planar, SN3D: what a field effect is handed.
std::vector<std::vector<float>> encoded(const std::vector<float>& mono, Vec3 from, int order, float gain = 1.0f) {
    std::vector<double> y(static_cast<std::size_t>(numChannels(order)));
    shSN3D(from, order, y);
    std::vector<std::vector<float>> field(y.size(), std::vector<float>(mono.size()));
    for (std::size_t c = 0; c < y.size(); ++c)
        for (std::size_t i = 0; i < mono.size(); ++i) field[c][i] = gain * static_cast<float>(y[c]) * mono[i];
    return field;
}

/// Level at the end of a field, heard through W with the field's power beside it, or through W alone.
double levelOf(const std::vector<std::vector<float>>& field, bool withPower) {
    std::vector<const float*> channels;
    for (const auto& c : field) channels.push_back(c.data());
    const int n = static_cast<int>(field[0].size());
    std::vector<float> power(static_cast<std::size_t>(n));
    fieldPower(channels, n, power.data());
    FeatureBank fb;
    fb.prepare(kSr);
    fb.process(field[0].data(), withPower ? power.data() : nullptr, n);
    return fb.value(Feature::Level);
}

}  // namespace

/*  Catches the normalisation going -- a sum of squares over (N+1)^2 channels reads 6 dB hot at
 *  order 3 and 9 dB at order 7, and every calibrated range in FeatureConfig is then wrong for a
 *  field -- and one by the channel count rather than its root, which reads as far too quiet. */
TEST_CASE("one source in a field reads the level W alone reads, at every order and from anywhere") {
    const auto mono = sine(300.0, 0.5, 0.25f);
    for (const int order : {1, 3, 7})
        for (const Vec3 from : {Vec3{1, 0, 0}, Vec3{0, 0, 1}, unit(Vec3{-0.3, 0.8, -0.5})}) {
            const auto field = encoded(mono, from, order);
            CAPTURE(order);
            CHECK(levelOf(field, true) == doctest::Approx(levelOf(field, false)).epsilon(1e-4));
        }
}

/*  Two sources in antiphase cancel in W, and a ducker listening to W hears silence under a loud
 *  field. Catches the power being taken and never read, and W's own square being read in its
 *  place. */
TEST_CASE("a field whose W cancels is still heard") {
    const auto mono = sine(300.0, 0.5, 0.25f);
    auto field = encoded(mono, Vec3{0, 1, 0}, 3);
    const auto other = encoded(mono, Vec3{0, -1, 0}, 3, -1.0f);
    for (std::size_t c = 0; c < field.size(); ++c)
        for (std::size_t i = 0; i < mono.size(); ++i) field[c][i] += other[c][i];

    CHECK(levelOf(field, false) == doctest::Approx(0.0));  // W is nothing at all
    const double single = levelOf(encoded(mono, Vec3{0, 1, 0}, 3), true);
    CHECK(levelOf(field, true) > single);  // and the field is two sources loud
}

/*  Catches the setter storing nothing, and one that moves the attack instead: both leave the fall
 *  where it was. The two are read at the same hop after the same burst.
 *
 *  The burst falls to a quiet tone, so this one is about the release alone; the case below is the
 *  same fall to digital silence, where the gate also comes into play.
 */
TEST_CASE("Level falls as fast as its release says") {
    auto burst = sine(300.0, 0.5, 0.5f);
    const auto tail = sine(300.0, 0.25, 0.01f);  // -43 dB RMS: Level 0.24 once it has settled
    burst.insert(burst.end(), tail.begin(), tail.end());

    const auto after = [&](double releaseS) {
        FeatureBank fb;
        fb.prepare(kSr);
        fb.setLevelRelease(releaseS);
        fb.process(burst.data(), static_cast<int>(burst.size()));
        return fb.value(Feature::Level);
    };
    const double fast = after(0.030), slow = after(1.5);
    CHECK(fast < 0.30);
    CHECK(slow > 0.60);

    FeatureBank fb;
    fb.prepare(kSr);
    fb.setLevelRelease(99.0);
    CHECK(fb.levelRelease() == doctest::Approx(kLevelReleaseMaxS));
    fb.setLevelRelease(0.0);
    CHECK(fb.levelRelease() == doctest::Approx(kLevelReleaseMinS));
}

/*  Catches the guard going: a follower is a one-pole, and a NaN never leaves one. Found by review --
 *  with a field, any of up to 121 channels can carry the sample, and Level drives the ducker. */
TEST_CASE("one bad sample does not silence the detector for good") {
    auto x = sine(300.0, 0.5, 0.25f);
    std::vector<float> power(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) power[i] = x[i] * x[i];
    power[1000] = std::numeric_limits<float>::quiet_NaN();  // in the power: W itself is clean

    FeatureBank fb;
    fb.prepare(kSr);
    fb.process(x.data(), power.data(), static_cast<int>(x.size()));
    CHECK(std::isfinite(fb.value(Feature::Level)));
    CHECK(fb.value(Feature::Level) > 0.5);
    CHECK(std::isfinite(fb.value(Feature::Attack)));

    fb.setLevelRelease(std::numeric_limits<double>::quiet_NaN());
    CHECK(std::isfinite(fb.levelRelease()));
}

/*  Catches Level being multiplied by the silence gate again: the gate shuts in 15 ms, so at
 *  digital silence the release would do nothing and a ducked wet would come back the moment a
 *  clip ended. */
TEST_CASE("Level's fall to digital silence is its release too, not the gate's") {
    auto burst = sine(300.0, 0.5, 0.5f);
    burst.resize(burst.size() + static_cast<std::size_t>(0.25 * kSr), 0.0f);  // 250 ms of nothing

    const auto after = [&](double releaseS) {
        FeatureBank fb;
        fb.prepare(kSr);
        fb.setLevelRelease(releaseS);
        fb.process(burst.data(), static_cast<int>(burst.size()));
        return fb.value(Feature::Level);
    };
    CHECK(after(0.030) < 0.05);
    CHECK(after(1.5) > 0.5);

    //  and silence is still silence: given time, Level is nothing, with no gate to make it so
    burst.resize(burst.size() + static_cast<std::size_t>(3.0 * kSr), 0.0f);
    CHECK(after(0.200) == doctest::Approx(0.0));
}
