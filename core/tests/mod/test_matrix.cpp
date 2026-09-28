// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The matrix as the editor edits it, value formatting, and the trajectory shapes' parameter table.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
void setParam(PluginState& s, ParamId id, double v) { s.params[static_cast<std::size_t>(id)] = static_cast<float>(v); }
}  // namespace

TEST_CASE("each tab has six columns, each with its own amount parameter; only the LFOs are bipolar") {
    CHECK(sourceColumn(encodeMod(), MatrixTab::Features, 0).name == "level");
    CHECK(sourceColumn(encodeMod(), MatrixTab::Features, 0).amount == EncoderParam::ModAmountLevel);
    CHECK(sourceColumn(encodeMod(), MatrixTab::Sidechain, 5).name == "high");
    CHECK(sourceColumn(encodeMod(), MatrixTab::Sidechain, 5).amount == EncoderParam::ModAmountScHigh);
    CHECK(sourceColumn(encodeMod(), MatrixTab::Generators, 0).name == "lfo 1");
    CHECK(sourceColumn(encodeMod(), MatrixTab::Generators, 3).amount == EncoderParam::ModAmountEnv1);
    CHECK(sourceColumn(encodeMod(), MatrixTab::Generators, 5).amount == EncoderParam::ModAmountEnv3);

    CHECK(sourceColumn(encodeMod(), MatrixTab::Generators, 2).bipolar);
    CHECK_FALSE(sourceColumn(encodeMod(), MatrixTab::Generators, 3).bipolar);  // envelopes rest at zero
    CHECK_FALSE(sourceColumn(encodeMod(), MatrixTab::Features, 1).bipolar);

    CHECK(sourceColumn(encodeMod(), MatrixTab::Features, 6).amount == kNoParamId);
    CHECK(sourceColumn(encodeMod(), MatrixTab::Features, -1).amount == kNoParamId);
}

TEST_CASE("every source slot has its own amount, by name") {
    //  The amounts happen to sit in a row in the manifest, but the manifest is append-only and the
    //  next source's amount cannot join them there. Catches two entries of the table swapped.
    constexpr std::array<std::string_view, kNumSources> keys{
        "mod.amount.level",  "mod.amount.attack",   "mod.amount.tonal",     "mod.amount.low",      "mod.amount.mid",
        "mod.amount.high",   "mod.amount.sc_level", "mod.amount.sc_attack", "mod.amount.sc_tonal", "mod.amount.sc_low",
        "mod.amount.sc_mid", "mod.amount.sc_high",  "mod.amount.lfo1",      "mod.amount.lfo2",     "mod.amount.lfo3",
        "mod.amount.env1",   "mod.amount.env2",     "mod.amount.env3",      "mod.amount.region1"};
    for (int i = 0; i < kNumSources; ++i) CHECK(parameter(sourceAmount(i)).key == keys[static_cast<std::size_t>(i)]);
    CHECK(sourceAmount(-1) == kNoParamId);
    CHECK(sourceAmount(kNumSources) == kNoParamId);
}

TEST_CASE("a region's orientation, its rates and every setting of its shape are targets; its amount is not") {
    //  What turns a region, what opens and closes it, and its shape's settings. Catches
    //  softness dropped, a rate filed as an angle -- which would smooth it instead of integrating -- and
    //  the region's own amount made a target of itself.
    for (const ParamId id : {EncoderParam::Region1Yaw, EncoderParam::Region1Pitch, EncoderParam::Region1Roll})
        CHECK(destinationKind(id) == DestKind::DirectAngle);
    for (const ParamId id :
         {EncoderParam::Region1YawRate, EncoderParam::Region1PitchRate, EncoderParam::Region1RollRate})
        CHECK(destinationKind(id) == DestKind::Rate);
    for (const ParamId id :
         {EncoderParam::Region1Size, EncoderParam::Region1Softness, EncoderParam::Region1BandElevation,
          EncoderParam::Region1Thickness, EncoderParam::Region1Fill, EncoderParam::Region1DotSize,
          EncoderParam::Region1Coverage, EncoderParam::Region1Contrast, EncoderParam::Region1Detail})
        CHECK(destinationKind(id) == DestKind::DirectScalar);
    CHECK_FALSE(isModulationTarget(encodeMod(), EncoderParam::ModAmountRegion1));
}

TEST_CASE("only a parameter that takes modulation can be a target") {
    CHECK(isModulationTarget(encodeMod(), EncoderParam::MotionSpeed));
    CHECK(isModulationTarget(encodeMod(), EncoderParam::RenderWidth));
    CHECK_FALSE(isModulationTarget(encodeMod(), EncoderParam::MotionDirection));
    CHECK_FALSE(isModulationTarget(encodeMod(), EncoderParam::RenderWidthMax));
    CHECK_FALSE(isModulationTarget(encodeMod(), EncoderParam::Lfo1Rate));
    CHECK_FALSE(isModulationTarget(encodeMod(), kNoParamId));
}

TEST_CASE("setting a cell adds, updates, clamps and -- at zero -- removes it") {
    PluginState s{encodeParams()};
    REQUIRE(setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::MotionDisplace, 0.35));
    CHECK(s.matrix.size() == 1);
    CHECK(cellDepth(s, MatrixTab::Features, 1, EncoderParam::MotionDisplace) == doctest::Approx(0.35));

    REQUIRE(setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::MotionDisplace, 2.0));
    CHECK(s.matrix.size() == 1);
    CHECK(cellDepth(s, MatrixTab::Features, 1, EncoderParam::MotionDisplace) == doctest::Approx(1.0));

    REQUIRE(setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::MotionDisplace, -3.0));
    CHECK(cellDepth(s, MatrixTab::Features, 1, EncoderParam::MotionDisplace) == doctest::Approx(-1.0));

    REQUIRE(setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::MotionDisplace, 0.0));
    CHECK(s.matrix.empty());

    CHECK_FALSE(
        setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::MotionDirection, 0.5));  // not a target
    CHECK_FALSE(
        setCellDepth(encodeMod(), s, MatrixTab::Features, 6, EncoderParam::MotionSpeed, 0.5));  // no such column
    CHECK(s.matrix.empty());

    //  A hand-edited document with the same cell twice collapses to one.
    s.matrix = {{MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.2},
                {MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.4}};
    REQUIRE(setCellDepth(encodeMod(), s, MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.6));
    CHECK(s.matrix.size() == 1);
    CHECK(cellDepth(s, MatrixTab::Features, 0, EncoderParam::MotionSpeed) == doctest::Approx(0.6));
}

TEST_CASE("one row is always shown in each plugin, and what used to be shown is still a target") {
    const PluginState empty{encodeParams()};
    const auto rows = matrixTargets(encodeMod(), empty);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == EncoderParam::MotionSpeed);
    REQUIRE(echoMod().baseTargets.size() == 1);
    CHECK(echoMod().baseTargets[0] == echoParams().byKey("output.wet"));
    /*  A row always shown is not what makes a parameter a target. Catches: any of these dropped from its
        plugin's target table along with its row. */
    for (const ParamId id : {EncoderParam::MotionDisplace, EncoderParam::RenderWidth})
        CHECK(isModulationTarget(encodeMod(), id));
    for (const char* key : {"tap1.spin", "tap1.feedback", "output.wet"})
        CHECK(isModulationTarget(echoMod(), static_cast<ParamId>(echoParams().byKey(key))));
    for (const char* key : {"room.size", "room.decay", "output.wet"})
        CHECK(isModulationTarget(reverbMod(), static_cast<ParamId>(reverbParams().byKey(key))));

    //  A row is not a claim that anything is routed.
    for (const int id : encodeMod().baseTargets) CHECK_FALSE(targetHasDepth(empty, static_cast<ParamId>(id)));

    //  Giving one depth does not give it a second row, and taking it away does not take the row.
    PluginState s{encodeParams()};
    setCellDepth(encodeMod(), s, MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.5);
    CHECK(matrixTargets(encodeMod(), s).size() == encodeMod().baseTargets.size());
    CHECK(targetHasDepth(s, EncoderParam::MotionSpeed));
    setCellDepth(encodeMod(), s, MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.0);
    CHECK(matrixTargets(encodeMod(), s).size() == encodeMod().baseTargets.size());
    CHECK_FALSE(targetHasDepth(s, EncoderParam::MotionSpeed));
}

TEST_CASE("anything else earns its row with depth, in the order it first got some") {
    PluginState s{encodeParams()};
    setCellDepth(encodeMod(), s, MatrixTab::Generators, 0, EncoderParam::TransformYaw, 0.45);
    setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::RenderGain, 0.2);
    setCellDepth(encodeMod(), s, MatrixTab::Sidechain, 1, EncoderParam::TransformYaw, 0.3);  // again: no new row

    const auto rows = matrixTargets(encodeMod(), s);
    REQUIRE(rows.size() == encodeMod().baseTargets.size() + 2);
    CHECK(rows[encodeMod().baseTargets.size()] == EncoderParam::TransformYaw);
    CHECK(rows[encodeMod().baseTargets.size() + 1] == EncoderParam::RenderGain);

    CHECK(hasDepthElsewhere(s, EncoderParam::TransformYaw, MatrixTab::Generators));  // it has sidechain depth
    CHECK_FALSE(hasDepthElsewhere(s, EncoderParam::RenderGain, MatrixTab::Features));

    setCellDepth(encodeMod(), s, MatrixTab::Generators, 0, EncoderParam::TransformYaw, 0.0);
    setCellDepth(encodeMod(), s, MatrixTab::Sidechain, 1, EncoderParam::TransformYaw, 0.0);
    CHECK(matrixTargets(encodeMod(), s).size() ==
          encodeMod().baseTargets.size() + 1);  // no depth anywhere: that row goes
}

TEST_CASE("modulation reach is the range the engine will actually cover") {
    PluginState s{encodeParams()};
    setParam(s, EncoderParam::RenderWidth, 60.0);
    setParam(s, EncoderParam::ModAmountLevel, 0.5);
    setParam(s, EncoderParam::ModGlobalAmount, 0.8);
    setCellDepth(encodeMod(), s, MatrixTab::Features, 0, EncoderParam::RenderWidth, 0.25);

    Reach r = modulationReach(encodeMod(), s, EncoderParam::RenderWidth);
    CHECK(r.low == doctest::Approx(0.0));
    CHECK(r.high == doctest::Approx(0.25 * 0.5 * 0.8 * 180.0));  // 18 degrees

    //  The engine, fed its source at the top of its range, settles exactly there.
    ModulationEngine m;
    m.useManifest(encodeMod());
    m.prepare(48000.0);
    m.setState(s);
    Transport tp;
    const std::vector<double> loud{1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 2000; ++i) m.process(loud, {}, tp, 1.0 / 187.5);
    CHECK(m.destination(EncoderParam::RenderWidth) == doctest::Approx(60.0 + r.high));

    //  A negative depth reaches downward; an LFO reaches both ways.
    setCellDepth(encodeMod(), s, MatrixTab::Features, 1, EncoderParam::RenderWidth, -0.5);
    setCellDepth(encodeMod(), s, MatrixTab::Generators, 0, EncoderParam::RenderWidth, 0.1);
    r = modulationReach(encodeMod(), s, EncoderParam::RenderWidth);
    const double attack = -0.5 * 1.0 * 0.8 * 180.0, lfo = 0.1 * 1.0 * 0.8 * 180.0;
    CHECK(r.low == doctest::Approx(attack - lfo));
    CHECK(r.high == doctest::Approx(18.0 + lfo));

    CHECK(modulationReach(encodeMod(), s, EncoderParam::MotionDirection).high == 0.0);  // not a target
}

TEST_CASE("values are written the way the canvas writes them") {
    CHECK(formatParameter(EncoderParam::TransformYaw, 97.0) == "97\xc2\xb0");
    CHECK(formatParameter(EncoderParam::TransformYaw, -42.4) ==
          "\xe2\x88\x92"
          "42\xc2\xb0");
    CHECK(formatParameter(EncoderParam::TransformYaw, -0.2) == "0\xc2\xb0");  // never "-0"
    CHECK(formatParameter(EncoderParam::MotionSpeed, 90.0) == "90 \xc2\xb0/s");
    CHECK(formatParameter(EncoderParam::RenderGain, 0.0) == "0.0 dB");
    CHECK(formatParameter(EncoderParam::RenderGain, -6.04) ==
          "\xe2\x88\x92"
          "6.0 dB");
    CHECK(formatParameter(EncoderParam::TransformExtent, 0.72) == "0.72");
    CHECK(formatParameter(EncoderParam::MotionDirection, 1.0) == "reverse");
    CHECK(formatParameter(EncoderParam::MotionMode, 1.0) == "ping-pong");
    CHECK(formatParameter(EncoderParam::Lfo1Sync, 1.0) == "on");
    CHECK(formatParameter(EncoderParam::Lfo1Rate, 0.25) == "0.25 hz");
    CHECK(formatParameter(EncoderParam::Env1Attack, 12.0) == "12 ms");
}

TEST_CASE("every trajectory shape describes its parameters, under the names its state is saved with") {
    for (auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                   GeneratorType::Spiral}) {
        INFO(std::string(name(g)));
        const auto info = generatorParams(g);
        const auto names = generatorParamNames(g);
        REQUIRE(info.size() == names.size());
        std::array<double, kMaxGenParams> defaults{};
        generatorDefaults(g, defaults);
        for (std::size_t i = 0; i < info.size(); ++i) {
            CHECK(info[i].name == names[i]);
            CHECK(info[i].min < info[i].max);
            CHECK(defaults[i] >= info[i].min);
            CHECK(defaults[i] <= info[i].max);
            CHECK((info[i].unit == "deg" || info[i].unit.empty()));
        }
    }
}
