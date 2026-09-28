// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/control.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/echo/patterns.hpp"
#include "bambi/patch/state.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
PluginState applied(EchoPattern p, int* touched = nullptr) {
    const ParamManifest& m = echoParams();
    PluginState s{m};
    applyPattern(m, p, [&](int at, float normalised) {
        if (touched) ++*touched;
        s.params[static_cast<std::size_t>(at)] = fromNormalised(m, at, normalised);
    });
    return s;
}
EchoSettings settingsOf(const PluginState& s) { return echoSettingsFrom(echoParams(), s.params, s.regions[0].shape); }
}  // namespace

TEST_CASE("spiral is what a fresh Echo opens as") {
    //  Catches: a default in echo/params.hpp or the spiral row changed without the other.
    const ParamManifest& m = echoParams();
    const PluginState fresh{m};
    const PluginState spiral = applied(EchoPattern::Spiral);
    for (int i = 0; i < m.size(); ++i)
        CHECK(spiral.params[static_cast<std::size_t>(i)] == doctest::Approx(fresh.params[static_cast<std::size_t>(i)]));
    CHECK(PluginState{}.echoPattern == static_cast<int>(EchoPattern::Spiral));
    CHECK(kEchoPatternNames[static_cast<std::size_t>(EchoPattern::Spiral)] == "spiral");
}

TEST_CASE("a pattern writes the four taps' pattern keys and nothing else") {
    //  Catches: a key dropped from or added to applyPattern.
    int touched = 0;
    const PluginState s = applied(EchoPattern::PingPong, &touched);
    CHECK(touched == kEchoTaps * kEchoPatternKeys);
    const auto e = settingsOf(s);
    CHECK(e.taps[0].on);
    CHECK(e.taps[0].spinDeg == doctest::Approx(180.0));
    CHECK(e.taps[0].timing.steps == 3);
    CHECK_FALSE(e.taps[1].on);
    //  the level is the user's: a pattern leaves it where the fresh instance had it
    CHECK(e.taps[0].levelDb == doctest::Approx(EchoSettings::defaults().taps[0].levelDb));
}

TEST_CASE("cascade is two taps a step apart turning opposite ways, skewed") {
    const auto e = settingsOf(applied(EchoPattern::Cascade));
    CHECK(e.taps[0].on);
    CHECK(e.taps[1].on);
    CHECK(e.taps[1].timing.offsetSteps == 1);
    CHECK(e.taps[0].spinDeg == doctest::Approx(-e.taps[1].spinDeg));
    CHECK(e.taps[0].skew == doctest::Approx(0.3));
    CHECK_FALSE(e.taps[2].on);
}

TEST_CASE("single is one tap that does not turn, at the least feedback there is") {
    const auto e = settingsOf(applied(EchoPattern::Single));
    CHECK(e.taps[0].spinDeg == doctest::Approx(0.0));
    CHECK(e.taps[0].feedbackDb == doctest::Approx(echoParams()[echoParams().byKey("tap1.feedback")].min));
}
