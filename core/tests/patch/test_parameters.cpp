// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/region/shape.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"
#include "targets.hpp"

using namespace bambi;

/*  The frozen list.
 *
 *  Hosts persist automation against the parameter key, so renaming or removing one
 *  silently detaches every automation lane in every session a user has ever saved. This
 *  list exists so that doing it accidentally is impossible: any rename, removal or
 *  reorder fails the build, and adding a parameter requires deliberately appending here.
 *
 *  Appending is fine. Everything else is a compatibility break that needs a decision.
 */
constexpr std::array kFrozenKeys{
    "transform.yaw",
    "transform.pitch",
    "transform.roll",
    "transform.extent",
    "motion.speed",
    "motion.displace",
    "motion.direction",
    "motion.mode",
    "render.width",
    "render.width_min",
    "render.width_max",
    "render.gain",
    "input.trim",
    "mod.global_amount",
    "mod.amount.level",
    "mod.amount.attack",
    "mod.amount.tonal",
    "mod.amount.low",
    "mod.amount.mid",
    "mod.amount.high",
    "mod.amount.sc_level",
    "mod.amount.sc_attack",
    "mod.amount.sc_tonal",
    "mod.amount.sc_low",
    "mod.amount.sc_mid",
    "mod.amount.sc_high",
    "mod.amount.lfo1",
    "mod.amount.lfo2",
    "mod.amount.lfo3",
    "mod.amount.env1",
    "mod.amount.env2",
    "mod.amount.env3",
    "lfo1.rate",
    "lfo1.sync",
    "lfo1.div",
    "lfo1.shape",
    "lfo1.phase",
    "lfo2.rate",
    "lfo2.sync",
    "lfo2.div",
    "lfo2.shape",
    "lfo2.phase",
    "lfo3.rate",
    "lfo3.sync",
    "lfo3.div",
    "lfo3.shape",
    "lfo3.phase",
    "env1.attack",
    "env1.decay",
    "env1.sustain",
    "env1.release",
    "env2.attack",
    "env2.decay",
    "env2.sustain",
    "env2.release",
    "env3.attack",
    "env3.decay",
    "env3.sustain",
    "env3.release",
    "transform.yaw_rate",
    "transform.pitch_rate",
    "transform.roll_rate",
    "lfo1.retrigger",
    "lfo2.retrigger",
    "lfo3.retrigger",
    "rates.retrigger",
    "region1.yaw",
    "region1.pitch",
    "region1.roll",
    "region1.yaw_rate",
    "region1.pitch_rate",
    "region1.roll_rate",
    "region1.softness",
    "region1.size",
    "region1.band_elevation",
    "region1.thickness",
    "region1.fill",
    "region1.dot_size",
    "region1.side",
    "region1.coverage",
    "region1.contrast",
    "region1.detail",
    "region1.evolve",
    "mod.amount.region1",
    /*  A polarity knob per LFO and envelope, replacing an older amplitude and offset pair, plus a
            curve per timed envelope stage. The two they replace are removed, not appended over -- a
            rename or a removal is exactly what this list exists to make deliberate. */
    "lfo1.polarity",
    "lfo2.polarity",
    "lfo3.polarity",
    "env1.attack_curve",
    "env1.decay_curve",
    "env1.release_curve",
    "env1.polarity",
    "env2.attack_curve",
    "env2.decay_curve",
    "env2.release_curve",
    "env2.polarity",
    "env3.attack_curve",
    "env3.decay_curve",
    "env3.release_curve",
    "env3.polarity",
    //  What a stereo input puts on the sphere. Appended, as a new key must be.
    "input.mode",
    "input.spread",
    "input.offset",
    //  How long Level takes to fall, the input's and the sidechain's.
    "level.release",
    "sc_level.release",
    //  A parametric path's continuous settings, one key each.
    "orbit.tilt",
    "orbit.axis_az",
    "orbit.aperture",
    "lissajous.az_amount",
    "lissajous.el_amount",
    "lissajous.phase",
    "wave.el_amount",
    "wave.el_offset",
    "arc.centre_az",
    "arc.centre_el",
    "arc.length",
    "arc.heading",
    "spiral.from_el",
    "spiral.to_el",
    "spiral.start_az",
};

TEST_CASE("parameter keys are frozen") {
    REQUIRE(parameters().size() == kFrozenKeys.size());
    for (std::size_t i = 0; i < kFrozenKeys.size(); ++i) {
        INFO("index ", i);
        CHECK(parameters()[i].key == kFrozenKeys[i]);
    }
}

TEST_CASE("manifest is internally consistent") {
    const auto all = parameters();

    SUBCASE("id matches table position") {
        for (std::size_t i = 0; i < all.size(); ++i) CHECK(static_cast<std::size_t>(all[i].id) == i);
    }

    SUBCASE("keys are unique") {
        std::set<std::string> seen;
        for (const auto& p : all) {
            INFO("duplicate key ", std::string(p.key));
            CHECK(seen.insert(std::string(p.key)).second);
        }
    }

    SUBCASE("display names are unique too — a DAW lane list with two 'gain' rows is useless") {
        std::set<std::string> seen;
        for (const auto& p : all) {
            INFO("duplicate name ", std::string(p.name));
            CHECK(seen.insert(std::string(p.name)).second);
        }
    }

    SUBCASE("defaults lie inside their own range") {
        for (const auto& p : all) {
            INFO(std::string(p.key));
            CHECK(p.def >= p.min);
            CHECK(p.def <= p.max);
            CHECK(p.min < p.max);
        }
    }

    SUBCASE("choice parameters declare as many options as their range spans") {
        for (const auto& p : all) {
            if (p.type != ParamType::Choice) continue;
            INFO(std::string(p.key));
            CHECK(choiceCount(p.id) == static_cast<int>(p.max - p.min) + 1);
        }
    }

    SUBCASE("bool parameters span exactly 0..1") {
        for (const auto& p : all) {
            if (p.type != ParamType::Bool) continue;
            INFO(std::string(p.key));
            CHECK(p.min == 0.0f);
            CHECK(p.max == 1.0f);
        }
    }

    SUBCASE("keys are namespaced by a dot") {
        for (const auto& p : all) {
            INFO(std::string(p.key));
            CHECK(p.key.find('.') != std::string_view::npos);
        }
    }
}

TEST_CASE("lookup by key round-trips") {
    for (const auto& p : parameters()) CHECK(parameterByKey(p.key) == p.id);
    CHECK(parameterByKey("nope.not.here") == kNoParamId);
    CHECK(parameterByKey("") == kNoParamId);
}

TEST_CASE("clampToRange") {
    CHECK(clampToRange(EncoderParam::TransformYaw, 500.0f) == doctest::Approx(180.0f));
    CHECK(clampToRange(EncoderParam::TransformYaw, -500.0f) == doctest::Approx(-180.0f));
    CHECK(clampToRange(EncoderParam::TransformExtent, 0.35f) == doctest::Approx(0.35f));

    SUBCASE("choice and bool snap to integers") {
        CHECK(clampToRange(EncoderParam::MotionMode, 1.4f) == doctest::Approx(1.0f));
        CHECK(clampToRange(EncoderParam::MotionMode, 1.6f) == doctest::Approx(2.0f));
        CHECK(clampToRange(EncoderParam::Lfo1Sync, 0.4f) == doctest::Approx(0.0f));
    }

    SUBCASE("NaN falls back to the default rather than propagating") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (const auto& p : parameters()) {
            INFO(std::string(p.key));
            CHECK(clampToRange(p.id, nan) == doctest::Approx(p.def));
        }
    }
}

TEST_CASE("trajectory shapes are state, and each modulation source has one amount") {
    SUBCASE("no trajectory shape parameter is automatable — shape is state") {
        for (const auto& p : parameters()) {
            INFO(std::string(p.key));
            CHECK(!p.key.starts_with("shape."));
            CHECK(!p.key.starts_with("trajectory."));
        }
    }
    SUBCASE("every modulation source has exactly one amount") {
        int amounts = 0;
        for (const auto& p : parameters())
            if (p.key.starts_with("mod.amount.")) ++amounts;
        CHECK(amounts == 19);  // 6 features + 6 sidechain + 3 lfo + 3 env + the region
    }
}

TEST_CASE("an LFO's rate gives its travel to the slow end, and every other range stays linear") {
    using bambi::ParamId;
    CHECK(bambi::toNormalised(EncoderParam::Lfo1Rate, 0.25f) == doctest::Approx(0.5));  // the default, at mid-travel
    CHECK(bambi::toNormalised(EncoderParam::Lfo2Rate, 0.01f) == doctest::Approx(0.0));
    CHECK(bambi::toNormalised(EncoderParam::Lfo3Rate, 2.0f) == doctest::Approx(1.0));
    CHECK(bambi::toNormalised(EncoderParam::Lfo1Rate, 1.0f) > 0.75f);  // the fast end is squeezed, not the slow one
    for (int i = 0; i <= 8; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        CHECK(bambi::toNormalised(EncoderParam::Lfo1Rate, bambi::fromNormalised(EncoderParam::Lfo1Rate, t)) ==
              doctest::Approx(t).epsilon(1e-4));
    }
    CHECK(bambi::toNormalised(EncoderParam::TransformYaw, 0.0f) == doctest::Approx(0.5));
    CHECK(bambi::fromNormalised(EncoderParam::RenderWidth, 0.25f) == doctest::Approx(45.0));
    CHECK(bambi::toNormalised(EncoderParam::RenderWidth, 270.0f) ==
          doctest::Approx(1.5));  // linear: a reach past the end is kept
}

TEST_CASE("a region's settings default to what the manifest says") {
    //  RegionSettingsDeg cannot read the manifest -- region/ sits below patch/ -- so its defaults are
    //  a second copy of these numbers. This is what keeps the two from drifting.
    const bambi::RegionSettingsDeg def;
    using bambi::ParamId;
    CHECK(bambi::parameter(EncoderParam::Region1Yaw).def == def.yaw);
    CHECK(bambi::parameter(EncoderParam::Region1Pitch).def == def.pitch);
    CHECK(bambi::parameter(EncoderParam::Region1Roll).def == def.roll);
    CHECK(bambi::parameter(EncoderParam::Region1Softness).def == def.softness);
    CHECK(bambi::parameter(EncoderParam::Region1Size).def == def.size);
    CHECK(bambi::parameter(EncoderParam::Region1BandElevation).def == def.bandElevation);
    CHECK(bambi::parameter(EncoderParam::Region1Thickness).def == def.thickness);
    CHECK(bambi::parameter(EncoderParam::Region1Fill).def == def.fill);
    CHECK(bambi::parameter(EncoderParam::Region1DotSize).def == def.dotSize);
}

TEST_CASE("every parameter the encoder can modulate is named, and no other") {
    /*  `destinationKind` is a side table of the manifest built from key strings, where a misspelling
        is silently "not modulatable" -- a cell that would simply never move. This is the frozen list
        that replaces a compile-time check, and it is the same kind of guard the key list itself has.

        Catches: any key dropped, added, or moved between kinds; and a key misspelled in the table,
        which shows here as a parameter that lost its kind. */
    const ParamManifest& m = encodeParams();
    std::vector<test::Target> expected = {
        {"motion.speed", DestKind::Rate},
        {"transform.yaw_rate", DestKind::Rate},
        {"transform.pitch_rate", DestKind::Rate},
        {"transform.roll_rate", DestKind::Rate},
        {"motion.displace", DestKind::DirectAngle},
        {"transform.yaw", DestKind::DirectAngle},
        {"transform.pitch", DestKind::DirectAngle},
        {"transform.roll", DestKind::DirectAngle},
        {"transform.extent", DestKind::DirectScalar},
        {"render.width", DestKind::DirectScalar},
        {"render.gain", DestKind::DirectScalar},
        {"input.trim", DestKind::DirectScalar},
        //  the width of a stereo input may move; its mode may not, and is not here
        {"input.spread", DestKind::DirectScalar},
        {"input.offset", DestKind::DirectScalar},
        //  a parametric path's settings
        {"orbit.tilt", DestKind::DirectScalar},
        {"orbit.axis_az", DestKind::DirectAngle},
        {"orbit.aperture", DestKind::DirectScalar},
        {"lissajous.az_amount", DestKind::DirectScalar},
        {"lissajous.el_amount", DestKind::DirectScalar},
        {"lissajous.phase", DestKind::DirectAngle},
        {"wave.el_amount", DestKind::DirectScalar},
        {"wave.el_offset", DestKind::DirectScalar},
        {"arc.centre_az", DestKind::DirectAngle},
        {"arc.centre_el", DestKind::DirectScalar},
        {"arc.length", DestKind::DirectScalar},
        {"arc.heading", DestKind::DirectAngle},
        {"spiral.from_el", DestKind::DirectScalar},
        {"spiral.to_el", DestKind::DirectScalar},
        {"spiral.start_az", DestKind::DirectAngle},
    };
    for (auto& t : test::regionTargets("region1")) expected.push_back(t);
    test::checkTargets(m, expected);

    //  the smoothing each kind gets, and the one value that is not derived from its kind
    CHECK(m.smoothingOf(m.byKey("motion.speed")) == doctest::Approx(0.020));
    CHECK(m.smoothingOf(m.byKey("render.width")) == doctest::Approx(0.040));
    CHECK(m.smoothingOf(m.byKey("transform.yaw")) == doctest::Approx(720.0));
    CHECK(m.smoothingOf(m.byKey("motion.displace")) == doctest::Approx(4.0));
    CHECK(m.smoothingOf(m.byKey("motion.mode")) == doctest::Approx(0.0));

    //  the skew, which is the other side table
    CHECK(m.skewCentreOf(m.byKey("lfo1.rate")) == doctest::Approx(0.25f));
    CHECK(m.skewCentreOf(m.byKey("lfo3.rate")) == doctest::Approx(0.25f));
    CHECK(m.skewCentreOf(m.byKey("transform.yaw")) == doctest::Approx(0.0f));

    //  a manifest answers for a position outside it rather than reading past its tables
    CHECK(m.byKey("no.such.parameter") == kNoParam);
    CHECK(m.destKindOf(kNoParam) == DestKind::NotModulatable);
    CHECK(m.destKindOf(m.size()) == DestKind::NotModulatable);
    CHECK(m.smoothingOf(m.size()) == doctest::Approx(0.0));
    CHECK(m.skewCentreOf(-1) == doctest::Approx(0.0f));
}

TEST_CASE(
    "a time or a rate whose useful choices are short puts its centre at mid-travel, in every plugin that has it") {
    //  Centres for a time or rate control with few useful choices. Catches: any one of them lost
    //  again, in any plugin -- a key a plugin does not have is simply absent there, so the same list
    //  checks all three.
    struct Centre {
        const char* key;
        float value;
    };
    constexpr Centre kCentres[] = {
        {"lfo1.rate", 0.25f},         {"lfo3.rate", 0.25f},       {"level.release", 200.0f},
        {"sc_level.release", 200.0f}, {"env1.attack", 50.0f},     {"env2.decay", 300.0f},
        {"env3.release", 500.0f},     {"tap1.ms", 250.0f},        {"tap4.offset_ms", 250.0f},
        {"tap2.low_cut", 200.0f},     {"tap3.high_cut", 3000.0f}, {"room.distance", 5.0f},
        {"output.pre_delay", 40.0f},  {"input.low_cut", 100.0f},  {"input.high_cut", 6000.0f},
    };
    const ParamManifest* plugins[] = {&encodeParams(), &echoParams(), &reverbParams()};
    int found = 0;
    for (const auto* m : plugins)
        for (const auto& c : kCentres) {
            const int at = m->byKey(c.key);
            if (at == kNoParam) continue;
            ++found;
            CAPTURE(c.key);
            CHECK(toNormalised(*m, at, c.value) == doctest::Approx(0.5).epsilon(1e-4));
        }
    CHECK(found == 3 * 7 + 8);  // seven shared keys in all three; eight of one plugin's own

    //  A key every plugin has skews the same in all three.
    for (int at = 0; at < encodeParams().size(); ++at) {
        const auto key = encodeParams()[at].key;
        for (const auto* m : {&echoParams(), &reverbParams()})
            if (const int there = m->byKey(key); there != kNoParam) {
                CAPTURE(std::string(key));
                CHECK(m->skewCentreOf(there) == encodeParams().skewCentreOf(at));
            }
    }
}
