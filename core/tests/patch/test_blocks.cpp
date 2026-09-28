// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/patch/readparams.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  The three lists, and the promises the shared blocks make about them.
 *
 *  The point of a block is that a host automation lane written for one plugin's region can be pasted
 *  onto another's. That is only true if the keys, the names' shape, the ranges and the defaults are
 *  identical, and only a test can hold three lists to it.
 */
namespace {

std::map<std::string, const ParamDesc*> byKey(const ParamManifest& m) {
    std::map<std::string, const ParamDesc*> out;
    for (const ParamDesc& d : m.descs) out.emplace(std::string(d.key), &d);
    return out;
}

/// Every key under a prefix, with the prefix removed: "region2.size" -> "size".
std::map<std::string, const ParamDesc*> block(const ParamManifest& m, std::string_view prefix) {
    std::map<std::string, const ParamDesc*> out;
    for (const ParamDesc& d : m.descs)
        if (d.key.size() > prefix.size() && d.key.starts_with(prefix))
            out.emplace(std::string(d.key.substr(prefix.size())), &d);
    return out;
}

void sameShape(const std::map<std::string, const ParamDesc*>& a, const std::map<std::string, const ParamDesc*>& b,
               const char* what, bool compareDefaults = true) {
    INFO(what);
    REQUIRE(a.size() == b.size());
    for (const auto& [suffix, da] : a) {
        INFO(suffix);
        const auto it = b.find(suffix);
        REQUIRE(it != b.end());
        const ParamDesc* db = it->second;
        CHECK(da->type == db->type);
        CHECK(da->unit == db->unit);
        CHECK(da->min == doctest::Approx(db->min));
        CHECK(da->max == doctest::Approx(db->max));
        if (compareDefaults) CHECK(da->def == doctest::Approx(db->def));
        CHECK(da->choices == db->choices);
    }
}

}  // namespace

TEST_CASE("every plugin's keys are unique, and its positions are its own manifest's") {
    //  Catches: a key typed twice in a list, which a manifest would resolve to the first and which
    //  would make one of the two silently unreachable.
    for (const ParamManifest* m : {&encodeParams(), &echoParams(), &reverbParams()}) {
        std::set<std::string_view> keys;
        for (int i = 0; i < m->size(); ++i) {
            const ParamDesc& d = (*m)[i];
            INFO(d.key);
            CHECK(keys.insert(d.key).second);
            CHECK(static_cast<int>(d.id) == i);  // a descriptor knows its own position
            CHECK(m->byKey(d.key) == i);
            CHECK_FALSE(d.name.empty());
        }
    }
}

TEST_CASE("a region block is the same block in every plugin, and in every slot") {
    /*  The whole promise of BAMBI_REGION_BLOCK: a lane written for Reverb's return pastes onto
        Echo's send because the keys, ranges and defaults are identical. Catches: a range edited in
        one plugin's list rather than in the macro -- which is exactly what a macro exists to stop,
        and what nobody would notice until two plugins disagreed in a session. */
    const auto encoder = block(encodeParams(), "region1.");
    const auto echoSend = block(echoParams(), "region1.");
    const auto reverbSend = block(reverbParams(), "region1.");
    const auto reverbReturn = block(reverbParams(), "region2.");

    REQUIRE(encoder.size() == 17);  // the twelve base settings, plus side, plus clouds' four
    sameShape(encoder, echoSend, "the encoder's region against Echo's send");
    sameShape(encoder, reverbSend, "the encoder's region against Reverb's send");
    sameShape(reverbSend, reverbReturn, "Reverb's two slots against each other");

    /*  And the block's own values, pinned. sameShape only catches two plugins disagreeing; a range
        edited in the macro itself moves all three together and nothing would notice, yet it changes
        what every plugin publishes to a host. Ranges may change -- the rules allow it -- but they
        must change deliberately, and this is what makes that so. */
    struct Pin {
        const char* key;
        ParamType type;
        double min, max, def;
    };
    const Pin kBlock[] = {
        {"yaw", ParamType::Float, -180, 180, 0},
        {"pitch", ParamType::Float, -180, 180, 0},
        {"roll", ParamType::Float, -180, 180, 0},
        {"yaw_rate", ParamType::Float, -180, 180, 0},
        {"pitch_rate", ParamType::Float, -180, 180, 0},
        {"roll_rate", ParamType::Float, -180, 180, 0},
        {"softness", ParamType::Float, 0, 180, 20},
        {"size", ParamType::Float, 0, 180, 45},
        {"band_elevation", ParamType::Float, -90, 90, 0},
        {"thickness", ParamType::Float, 0, 180, 30},
        {"fill", ParamType::Float, 0, 1, 0.5},
        {"dot_size", ParamType::Float, 0, 90, 20},
        {"side", ParamType::Choice, 0, 1, 0},
        {"coverage", ParamType::Float, 0, 1, 0.5},
        {"contrast", ParamType::Float, 0, 2, 0.6},
        {"detail", ParamType::Float, 0, 1, 0.5},
        {"evolve", ParamType::Float, -180, 180, 0},
    };
    REQUIRE(std::size(kBlock) == encoder.size());
    for (const Pin& p : kBlock) {
        INFO(p.key);
        const auto it = encoder.find(p.key);
        REQUIRE(it != encoder.end());
        CHECK(it->second->type == p.type);
        CHECK(it->second->min == doctest::Approx(p.min));
        CHECK(it->second->max == doctest::Approx(p.max));
        CHECK(it->second->def == doctest::Approx(p.def));
    }

    //  the names are by role, so they differ where the keys do not
    CHECK(byKey(echoParams())["region1.size"]->name == "send size");
    CHECK(byKey(reverbParams())["region2.size"]->name == "return size");
    CHECK(byKey(encodeParams())["region1.size"]->name == "region size");
}

TEST_CASE("the generators are the same six in every plugin, wherever they sit in the list") {
    /*  LFOs and envelopes are in every plugin and nothing about them is plugin-specific. The
        encoder's are scattered through its own list; Echo's and Reverb's are stamped whole from the
        shared block. Same keys, different positions -- which is the point, since everything is
        addressed by key.

        Catches: an envelope's range changed in the block but not in the encoder, or the other way
        round, which would make one plugin's saved patch load wrong in another. */
    for (const char* prefix : {"lfo1.", "lfo2.", "lfo3.", "env1.", "env2.", "env3."}) {
        sameShape(block(encodeParams(), prefix), block(echoParams(), prefix), prefix);
        sameShape(block(encodeParams(), prefix), block(reverbParams(), prefix), prefix);
    }
    //  seven settings an LFO, eight an envelope, including its polarity and its curves
    CHECK(block(echoParams(), "lfo1.").size() == 7);
    CHECK(block(echoParams(), "env1.").size() == 8);

    //  and the encoder's really are somewhere else in its list, so the test is not vacuous
    const int encoderLfo1 = encodeParams().byKey("lfo1.rate"), echoLfo1 = echoParams().byKey("lfo1.rate");
    REQUIRE(encoderLfo1 != kNoParam);
    REQUIRE(echoLfo1 != kNoParam);
    CHECK(encoderLfo1 != echoLfo1);
}

TEST_CASE("every plugin carries the matrix's amounts, one a source slot and one a region slot") {
    //  Catches: a plugin missing an amount, which is a source a host can never reach.
    for (const ParamManifest* m : {&encodeParams(), &echoParams(), &reverbParams()}) {
        CHECK(m->byKey("mod.global_amount") != kNoParam);
        for (const char* k :
             {"mod.amount.level", "mod.amount.sc_high", "mod.amount.lfo3", "mod.amount.env3", "mod.amount.region1"})
            CHECK(m->byKey(k) != kNoParam);
    }
    //  Reverb has two region slots, so it has two amounts; the others have one
    CHECK(reverbParams().byKey("mod.amount.region2") != kNoParam);
    CHECK(echoParams().byKey("mod.amount.region2") == kNoParam);
    CHECK(encodeParams().byKey("mod.amount.region2") == kNoParam);
}

TEST_CASE("each list is what its page specifies, and fits the cap the bus shares") {
    //  Catches: the cap lowered, or a list grown past it. The static_asserts in each params.cpp
    //  catch it at compile time; this says what the three actually are, so a change is visible.
    CHECK(encodeParams().size() == 119);  // level and sidechain release, clouds' four, the path's fifteen
    CHECK(kNumEchoParams == 151);         // + the same two; + clouds' four
    CHECK(kNumReverbParams == 114);       // + the same two; + clouds' four in each of two slots; - room.shape
    CHECK(kNumEchoParams <= kMaxParams);

    //  Echo: four taps of sixteen
    for (const char* tap : {"tap1.", "tap2.", "tap3.", "tap4."}) CHECK(block(echoParams(), tap).size() == 16);
    /*  The same keys, types and ranges -- but not the same defaults: the spec opens "tap 1 on, the
        rest off", each on its own step count and turning its own way, so the block takes those six
        as arguments. A test that compared defaults here would forbid the spec. */
    sameShape(block(echoParams(), "tap1."), block(echoParams(), "tap4."), "Echo's first tap against its fourth", false);

    //  Reverb: the room, the output, the input and the quality switch
    for (const char* k : {"room.size", "room.decay", "room.tone", "room.roughness", "room.distance", "output.wet",
                          "output.dry", "output.pre_delay", "input.low_cut", "input.high_cut", "quality.playing"})
        CHECK(reverbParams().byKey(k) != kNoParam);
    //  and what is not a control
    //  -- and the shape, which is the room's and rides with it in state
    for (const char* k : {"room.shape", "room.absorption", "room.mixing_time", "tail.lines", "tail.order",
                          "quality.render", "duck.amount", "output.mix"})
        CHECK(reverbParams().byKey(k) == kNoParam);

    /*  Every effect has the same output stage, and the encoder has none: it is a source, and what it
        renders is the signal. Catches: a mix parameter added beside the two levels, which
        would be a second source of truth for the same two gains. */
    for (const ParamManifest* m : {&echoParams(), &reverbParams()}) {
        CHECK(m->byKey("output.wet") != kNoParam);
        CHECK(m->byKey("output.dry") != kNoParam);
        CHECK(m->byKey("output.mix") == kNoParam);
        CHECK((*m)[m->byKey("output.dry")].def == doctest::Approx(0.0));  // an insert passes its input
        CHECK((*m)[m->byKey("output.wet")].def == doctest::Approx(0.0));
    }
    CHECK(encodeParams().byKey("output.dry") == kNoParam);
}

TEST_CASE("what wraps is named in full, in every plugin") {
    /*  Decided by key strings, where a slip is silent. Catches: a rate made to wrap, a tap's elevation
        made to wrap, displace left clamped. */
    const auto wrapping = [](const ParamManifest& m) {
        std::vector<std::string> keys;
        for (int i = 0; i < m.size(); ++i)
            if (m.wrapsAt(i)) keys.emplace_back(m[i].key);
        std::sort(keys.begin(), keys.end());
        return keys;
    };
    const auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto region = [](const std::string& s) {
        return std::vector<std::string>{s + ".yaw", s + ".pitch", s + ".roll", s + ".evolve"};
    };
    std::vector<std::string> lfos{"lfo1.phase", "lfo2.phase", "lfo3.phase"};

    auto encoder = region("region1");
    encoder.insert(encoder.end(), lfos.begin(), lfos.end());
    encoder.insert(encoder.end(),
                   {"transform.yaw", "transform.pitch", "transform.roll", "motion.displace", "orbit.axis_az",
                    "lissajous.phase", "arc.centre_az", "arc.heading", "spiral.start_az"});
    CHECK(wrapping(encodeParams()) == sorted(encoder));

    auto echo = region("region1");
    echo.insert(echo.end(), lfos.begin(), lfos.end());
    for (int t = 1; t <= 4; ++t)
        echo.insert(echo.end(), {"tap" + std::to_string(t) + ".az", "tap" + std::to_string(t) + ".spin"});
    CHECK(wrapping(echoParams()) == sorted(echo));

    auto reverb = region("region1");
    const auto second = region("region2");
    reverb.insert(reverb.end(), second.begin(), second.end());
    reverb.insert(reverb.end(), lfos.begin(), lfos.end());
    CHECK(wrapping(reverbParams()) == sorted(reverb));
}

TEST_CASE("every key fits the buffer keys are built in without allocation") {
    //  Catches: a key longer than keyOf's buffer, which would be truncated and read as its fallback.
    for (const ParamManifest* m : {&encodeParams(), &echoParams(), &reverbParams()})
        for (const ParamDesc& d : m->descs) {
            INFO(std::string(d.key));
            CHECK(d.key.size() < kMaxKeyChars);
        }
}

TEST_CASE("every region setting is a modulation target, or is named as not one") {
    //  Catches: a field added to the region block and not to sharedDestKind, which would silently
    //  stay out of the matrix.
    const std::set<std::string> notTargets{"side"};
    for (const auto& [field, d] : block(encodeParams(), "region1.")) {
        INFO(field);
        CHECK((sharedDestKind(d->key) != DestKind::NotModulatable) != (notTargets.count(field) == 1));
    }
}

TEST_CASE("a choice parameter lists one choice for every value of its range") {
    for (const ParamManifest* m : {&encodeParams(), &echoParams(), &reverbParams()})
        for (const ParamDesc& d : m->descs) {
            if (d.type != ParamType::Choice) continue;
            INFO(std::string(d.key));
            const auto listed = 1 + std::count(d.choices.begin(), d.choices.end(), ',');
            CHECK(!d.choices.empty());
            CHECK(listed == static_cast<long>(d.max - d.min) + 1);
        }
}
