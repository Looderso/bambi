// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "../doctest.h"
#include "../patch/targets.hpp"
#include "bambi/reverb/params.hpp"

namespace bambi {

TEST_CASE("Reverb's modulation manifest names a real parameter for every role") {
    //  As Echo's. Catches: a misspelled generator key, a missing source amount, or a base target
    //  that is not a key.
    CHECK(reverbMod().complete());
    CHECK(reverbMod().params == &reverbParams());
    CHECK(reverbMod().lfos[2].polarity == reverbParams().byKey("lfo3.polarity"));
    CHECK(reverbMod().amountOf(18) == reverbParams().byKey("mod.amount.region1"));
    /*  The row always shown is distance, which changes gains only. Catches: the room's size
        back in its place -- a target whose every move fades one network into the next. */
    REQUIRE(reverbMod().baseTargets.size() == 1);
    CHECK(reverbMod().baseTargets[0] == reverbParams().byKey("room.distance"));
}

TEST_CASE("every parameter Reverb can modulate is named, and no other") {
    //  Catches: any key dropped, added, misspelled or moved between kinds.
    std::vector<test::Target> expected = {
        {"room.size", DestKind::DirectScalar},     {"room.decay", DestKind::DirectScalar},
        {"room.tone", DestKind::DirectScalar},     {"room.roughness", DestKind::DirectScalar},
        {"room.distance", DestKind::DirectScalar}, {"output.wet", DestKind::DirectScalar},
        {"output.dry", DestKind::DirectScalar},    {"output.pre_delay", DestKind::DirectScalar},
        {"input.low_cut", DestKind::DirectScalar}, {"input.high_cut", DestKind::DirectScalar},
    };
    for (const char* slot : {"region1", "region2"})
        for (auto& t : test::regionTargets(slot)) expected.push_back(t);
    test::checkTargets(reverbParams(), expected);
}

}  // namespace bambi
