// SPDX-License-Identifier: GPL-3.0-or-later
#include <set>
#include <string>

#include "bambi/encode/params.hpp"
#include "bambi/mod/manifest.hpp"
#include "doctest.h"

using namespace bambi;

/*  The shared modulation engine's view of a plugin's parameters. Every role is bound by key
 *  and never by stride, because a generator's parameters are appended at the end of the list and so
 *  are never contiguous -- one LFO's settings are spread across several separate runs.
 */

TEST_CASE("the encoder's modulation manifest names a real parameter for every role") {
    /*  Catches: any key misspelled in manifest.cpp. Reaching a generator by stride, a misspelling was
        impossible -- the enum member either existed or the build failed. Bound by key it is a
        generator that silently never moves, so `complete()` is what the compiler used to be. */
    const ModManifest& m = encodeMod();
    REQUIRE(m.params != nullptr);
    CHECK(m.complete());
    CHECK(m.lfos.size() == 3);
    CHECK(m.envelopes.size() == 3);
    CHECK(m.sourceAmount.size() == 19);
    CHECK(m.baseTargets.size() == 1);  // one row always shown
}

TEST_CASE("every role resolves to the parameter whose key says so, and no two roles share one") {
    /*  Catches: two roles bound to the same key -- lfo2.rate given lfo1's, say -- which `complete()`
        alone would pass, every field being a real parameter. A generator would then follow another's
        setting instead of its own. */
    const ModManifest& m = encodeMod();
    const ParamManifest& p = *m.params;
    std::set<int> seen;
    const auto once = [&](int at, const char* expectedKey) {
        INFO(expectedKey);
        REQUIRE(at != kNoParam);
        CHECK(p[at].key == expectedKey);
        CHECK(seen.insert(at).second);  // nothing is bound twice
    };

    for (int i = 0; i < 3; ++i) {
        const LfoParams& l = m.lfos[static_cast<std::size_t>(i)];
        const std::string n = "lfo" + std::to_string(i + 1);
        once(l.rate, (n + ".rate").c_str());
        once(l.sync, (n + ".sync").c_str());
        once(l.div, (n + ".div").c_str());
        once(l.shape, (n + ".shape").c_str());
        once(l.phase, (n + ".phase").c_str());
        once(l.retrigger, (n + ".retrigger").c_str());
        once(l.polarity, (n + ".polarity").c_str());
    }
    for (int i = 0; i < 3; ++i) {
        const EnvParams& e = m.envelopes[static_cast<std::size_t>(i)];
        const std::string n = "env" + std::to_string(i + 1);
        once(e.attack, (n + ".attack").c_str());
        once(e.attackCurve, (n + ".attack_curve").c_str());
        once(e.decayCurve, (n + ".decay_curve").c_str());
        once(e.releaseCurve, (n + ".release_curve").c_str());
        once(e.polarity, (n + ".polarity").c_str());
        once(e.decay, (n + ".decay").c_str());
        once(e.sustain, (n + ".sustain").c_str());
        once(e.release, (n + ".release").c_str());
    }
    for (int s = 0; s < static_cast<int>(m.sourceAmount.size()); ++s) {
        INFO("slot ", s);
        REQUIRE(m.amountOf(s) != kNoParam);
        CHECK(seen.insert(m.amountOf(s)).second);
    }
    once(m.globalAmount, "mod.global_amount");
}

TEST_CASE("a generator's parameters are not contiguous, which is why nothing is reached by stride") {
    /*  Not a property of the code but of the key list, asserted so that the reason the manifest exists
        stays visible: keys are appended at the end and never inserted, so one LFO's settings are
        spread over the list in separate runs. A stride that happens to work today is a fact about
        where the last append landed.

        Catches: nothing directly -- this fails the day someone "tidies" the list by moving a key,
        which is the one edit the frozen-key rules forbid. */
    const ModManifest& m = encodeMod();
    const LfoParams& l = m.lfos[0];
    CHECK(l.retrigger > l.phase + 1);     // a run of its own, appended later
    CHECK(l.polarity > l.retrigger + 1);  // and another

    //  and the three LFOs are not a fixed stride apart in every run either
    const int rateStride = m.lfos[1].rate - m.lfos[0].rate;
    const int polarityStride = m.lfos[1].polarity - m.lfos[0].polarity;
    CHECK(rateStride != polarityStride);
}

TEST_CASE("a role a plugin does not have reads as absent, not as the first parameter") {
    //  Catches: kNoParam made 0. Every unbound role would then silently read parameter 0.
    CHECK(kNoParam < 0);
    const ModManifest empty;
    CHECK_FALSE(empty.complete());
    CHECK(empty.amountOf(0) == kNoParam);
    CHECK(empty.amountOf(-1) == kNoParam);
    CHECK(empty.amountOf(99) == kNoParam);
    const LfoParams unbound;
    CHECK(unbound.rate == kNoParam);
    CHECK(unbound.polarity == kNoParam);
}
