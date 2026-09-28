// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "../patch/targets.hpp"
#include "bambi/echo/params.hpp"
#include "doctest.h"

using namespace bambi;

TEST_CASE("Echo's modulation manifest names a real parameter for every role") {
    //  Catches: a misspelled generator key, a missing source amount, or a base target that is not
    //  a key -- each leaves a kNoParam that complete() refuses.
    CHECK(echoMod().complete());
    CHECK(echoMod().params == &echoParams());
    //  and the shared builder found THIS plugin's positions, not the encoder's
    CHECK(echoMod().lfos[0].rate == echoParams().byKey("lfo1.rate"));
    CHECK(echoMod().amountOf(18) == echoParams().byKey("mod.amount.region1"));
    for (const char* key : {"tap1.spin", "tap1.feedback", "output.wet"})
        CHECK(echoParams().byKey(key) != kNoParam);  // the base targets it names
}

TEST_CASE("every parameter Echo can modulate is named, and no other") {
    //  Catches: any key dropped, added, misspelled or moved between kinds -- and a loose suffix
    //  match that would make `mod.amount.level` a target.
    std::vector<test::Target> expected = {
        {"output.wet", DestKind::DirectScalar},
        {"output.dry", DestKind::DirectScalar},
    };
    for (const char* tap : {"tap1", "tap2", "tap3", "tap4"}) {
        const std::string t = tap;
        for (auto target : std::vector<test::Target>{{t + ".spin", DestKind::Rate},
                                                     {t + ".az", DestKind::DirectAngle},
                                                     {t + ".el", DestKind::DirectAngle},
                                                     {t + ".level", DestKind::DirectScalar},
                                                     {t + ".feedback", DestKind::DirectScalar},
                                                     {t + ".skew", DestKind::DirectScalar},
                                                     {t + ".blur", DestKind::DirectScalar},
                                                     {t + ".low_cut", DestKind::DirectScalar},
                                                     {t + ".high_cut", DestKind::DirectScalar}})
            expected.push_back(target);
    }
    for (auto& t : test::regionTargets("region1")) expected.push_back(t);
    test::checkTargets(echoParams(), expected);
    CHECK(echoParams().destKindOf(echoParams().byKey("mod.amount.level")) == DestKind::NotModulatable);
}
