// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/echo/params.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/mod/modulation.hpp"
#include "bambi/mod/regionclip.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

void set(PluginState& s, const ParamManifest& m, const char* key, float value) {
    const int at = m.byKey(key);
    REQUIRE(at != kNoParam);
    s.params[static_cast<std::size_t>(at)] = value;
}

/// The value a paste would write to `key`, or NaN when it writes nothing there.
float written(const std::vector<std::pair<int, float>>& writes, const ParamManifest& m, const char* key) {
    const int at = m.byKey(key);
    for (const auto& [where, value] : writes)
        if (where == at) return value;
    return std::numeric_limits<float>::quiet_NaN();
}

/// Echo's send: a band, turned, soft, its side set to outside and its amount halved -- both the use's.
PluginState echoWithARegion() {
    PluginState s{echoParams()};
    s.regions[0].shape = {RegionKind::Band, 5, 8};
    set(s, echoParams(), "region1.yaw", 40.0f);
    set(s, echoParams(), "region1.thickness", 25.0f);
    set(s, echoParams(), "region1.softness", 140.4f);
    set(s, echoParams(), "region1.yaw_rate", 12.0f);
    set(s, echoParams(), "region1.side", 1.0f);
    set(s, echoParams(), "mod.amount.region1", 0.5f);
    return s;
}

}  // namespace

/*  Catches the clip being keyed by key rather than by field -- `region1.yaw` means nothing to a
 *  slot called `region2` -- and the side or the amount travelling, which are the use's and would
 *  turn a send's region into a return's gate. */
TEST_CASE("a region copied from Echo's send lands on Reverb's return, without the use's side or amount") {
    const RegionClip clip = copyRegion(echoMod(), echoWithARegion(), 0, "region1");

    PluginState reverb{reverbParams()};
    std::vector<std::pair<int, float>> writes;
    pasteRegion(reverbMod(), clip, 1, "region2", reverb, writes);

    CHECK(reverb.regions[1].shape == RegionShape{RegionKind::Band, 5, 8});
    CHECK(reverb.regions[0].shape == PluginState{reverbParams()}.regions[0].shape);  // the other slot is not touched
    CHECK(written(writes, reverbParams(), "region2.yaw") == doctest::Approx(40.0f));
    CHECK(written(writes, reverbParams(), "region2.thickness") == doctest::Approx(25.0f));
    CHECK(written(writes, reverbParams(), "region2.yaw_rate") == doctest::Approx(12.0f));
    CHECK(std::isnan(written(writes, reverbParams(), "region2.side")));
    CHECK(std::isnan(written(writes, reverbParams(), "mod.amount.region2")));
    CHECK(std::isnan(written(writes, reverbParams(), "region1.yaw")));
}

/*  Row by row: catches a row from an LFO set to continue travelling (it agrees with nothing), a
 *  row from an envelope travelling (it fires from another track's notes), and the LFO arriving
 *  without its settings, its amount or its index, which is the sample-and-hold seed. */
TEST_CASE("the rows that travel are those from LFOs set to restart, each with its LFO") {
    PluginState s = echoWithARegion();
    const ParamManifest& m = echoParams();
    const auto target = [&](const char* key) { return static_cast<ParamId>(m.byKey(key)); };
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Generators, 1, target("region1.yaw"), 0.5));   // lfo 2, restart
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Generators, 2, target("region1.size"), 0.3));  // lfo 3, continue
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Generators, kNumLfos, target("region1.pitch"), 0.4));  // envelope 1
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Features, 0, target("region1.roll"), 0.2));            // level
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Generators, 1, target("tap1.spin"), 0.9));  // not the region's
    set(s, m, "lfo3.retrigger", 1.0f);
    set(s, m, "lfo2.rate", 1.5f);
    set(s, m, "lfo2.shape", 3.0f);
    set(s, m, "lfo2.polarity", 0.25f);
    set(s, m, "mod.amount.lfo2", 0.6f);

    const RegionClip clip = copyRegion(echoMod(), s, 0, "region1");
    REQUIRE(clip.rows.size() == 1);
    CHECK(clip.rows[0].lfo == 1);
    CHECK(clip.rows[0].field == "yaw");
    CHECK(clip.rows[0].depth == doctest::Approx(0.5));
    REQUIRE(clip.lfos.size() == 1);
    CHECK(clip.lfos[0].index == 1);
    CHECK(clip.lfos[0].rate == doctest::Approx(1.5f));
    CHECK(clip.lfos[0].shape == doctest::Approx(3.0f));
    CHECK(clip.lfos[0].polarity == doctest::Approx(0.25f));
    CHECK(clip.lfos[0].amount == doctest::Approx(0.6f));
}

/*  Catches a paste leaving an LFO row the clip does not have -- the region would go on being moved
 *  by it, and the two instances would no longer agree -- and a paste taking rows that are this
 *  instance's own: a feature's, an envelope's. */
TEST_CASE("a paste replaces the LFO rows onto its region, and leaves the instance's own") {
    PluginState from = echoWithARegion();
    REQUIRE(setCellDepth(echoMod(), from, MatrixTab::Generators, 0,
                         static_cast<ParamId>(echoParams().byKey("region1.yaw")), 0.5));
    set(from, echoParams(), "lfo1.rate", 0.75f);
    const RegionClip clip = copyRegion(echoMod(), from, 0, "region1");

    PluginState into{reverbParams()};
    const ParamManifest& m = reverbParams();
    const auto target = [&](const char* key) { return static_cast<ParamId>(m.byKey(key)); };
    REQUIRE(setCellDepth(reverbMod(), into, MatrixTab::Generators, 2, target("region2.size"), 0.8));  // goes
    REQUIRE(setCellDepth(reverbMod(), into, MatrixTab::Features, 0, target("region2.size"), 0.3));    // stays
    REQUIRE(
        setCellDepth(reverbMod(), into, MatrixTab::Generators, 2, target("region1.size"), 0.7));  // the other slot's

    std::vector<std::pair<int, float>> writes;
    pasteRegion(reverbMod(), clip, 1, "region2", into, writes);

    CHECK(cellDepth(into, MatrixTab::Generators, 2, target("region2.size")) == doctest::Approx(0.0));
    CHECK(cellDepth(into, MatrixTab::Features, 0, target("region2.size")) == doctest::Approx(0.3));
    CHECK(cellDepth(into, MatrixTab::Generators, 2, target("region1.size")) == doctest::Approx(0.7));
    CHECK(cellDepth(into, MatrixTab::Generators, 0, target("region2.yaw")) == doctest::Approx(0.5));
    CHECK(written(writes, m, "lfo1.rate") == doctest::Approx(0.75f));
    CHECK(written(writes, m, "lfo1.retrigger") == doctest::Approx(0.0f));
}

/*  Catches the version check going, or the parser guessing: a clipboard holds anything, and what is
 *  not a region descriptor of this version is nothing. */
TEST_CASE("what is on the clipboard is a current region descriptor, or it is nothing") {
    const std::string good = writeRegionClip(copyRegion(echoMod(), echoWithARegion(), 0, "region1"));
    REQUIRE(readRegionClip(good).has_value());

    CHECK_FALSE(readRegionClip("").has_value());
    CHECK_FALSE(readRegionClip("the quick brown fox").has_value());
    CHECK_FALSE(readRegionClip(R"({"kind":"band","fields":{}})").has_value());  // says nothing of what it is
    CHECK_FALSE(readRegionClip(R"({"bambi.region":2,"kind":"band","fields":{}})").has_value());  // a newer one
    CHECK_FALSE(
        readRegionClip(R"({"bambi.region":1,"kind":"cloud","fields":{}})").has_value());  // a kind this build has not
    CHECK_FALSE(readRegionClip(R"({"bambi.region":1,"kind":"band"})").has_value());       // no settings
    /*  Each of these is a guess the reader could make and must not: a version that rounds to
        this one, a region with settings missing, an LFO with settings missing, a row from an LFO
        that did not come with it, a row onto a field a region does not have. */
    const auto mangled = [&](const std::string& from, const std::string& to) {
        std::string text = good;
        const auto where = text.find(from);
        REQUIRE(where != std::string::npos);
        text.replace(where, from.size(), to);
        return readRegionClip(text).has_value();
    };
    CHECK_FALSE(mangled("\"bambi.region\":1", "\"bambi.region\":1.9"));
    CHECK_FALSE(mangled("\"yaw\":40", "\"yaws\":40"));
    CHECK_FALSE(mangled("\"yaw\":40", "\"yaw\":\"north\""));
    CHECK_FALSE(readRegionClip(R"({"bambi.region":1,"kind":"band","fields":{}})").has_value());
    CHECK_FALSE(readRegionClip(good + std::string(20000, ' ')).has_value());  // not ours to parse

    PluginState rowed = echoWithARegion();
    REQUIRE(setCellDepth(echoMod(), rowed, MatrixTab::Generators, 0,
                         static_cast<ParamId>(echoParams().byKey("region1.yaw")), 0.5));
    const std::string withRow = writeRegionClip(copyRegion(echoMod(), rowed, 0, "region1"));
    REQUIRE(readRegionClip(withRow).has_value());
    const auto mangledRow = [&](const std::string& from, const std::string& to) {
        std::string text = withRow;
        const auto where = text.find(from);
        REQUIRE(where != std::string::npos);
        text.replace(where, from.size(), to);
        return readRegionClip(text).has_value();
    };
    CHECK_FALSE(mangledRow("\"lfo\":0", "\"lfo\":1"));                   // a row from an LFO that did not come with it
    CHECK_FALSE(mangledRow("\"field\":\"yaw\"", "\"field\":\"side\""));  // onto what is not a region's
    CHECK_FALSE(mangledRow("\"polarity\":", "\"polarty\":"));            // an LFO with a setting missing
}

/*  Catches a field lost or renamed between the writer and the reader, and a side smuggled in by
 *  hand: text is text, and a clip that names `side` must still not carry it. */
TEST_CASE("a clip survives the clipboard, and a side written into one by hand does not") {
    PluginState s = echoWithARegion();
    REQUIRE(setCellDepth(echoMod(), s, MatrixTab::Generators, 0,
                         static_cast<ParamId>(echoParams().byKey("region1.yaw")), -0.25));
    const RegionClip out = copyRegion(echoMod(), s, 0, "region1");
    const auto back = readRegionClip(writeRegionClip(out));
    REQUIRE(back.has_value());
    CHECK(back->shape == out.shape);
    CHECK(back->fields == out.fields);
    REQUIRE(back->rows.size() == 1);
    CHECK(back->rows[0].depth == doctest::Approx(-0.25));
    CHECK(describeRegionClip(*back) == "band, 78 % soft, sets lfo 1");

    //  text is text: a `side` written into a descriptor by hand makes it not a descriptor
    std::string smuggled = writeRegionClip(out);
    const auto at = smuggled.find("\"fields\":{");
    REQUIRE(at != std::string::npos);
    smuggled.insert(at + std::string("\"fields\":{").size(), "\"side\":1,");
    CHECK_FALSE(readRegionClip(smuggled).has_value());
}
