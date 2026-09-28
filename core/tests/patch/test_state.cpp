// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <string>

#include "bambi/encode/params.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/trajectory.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

PluginState populated() {
    PluginState s{encodeParams()};
    s.identity = Identity::create();
    s.identity.label = "rhodes";
    s.identity.colour = 3;

    s.trajectory.kind = TrajectoryKind::Custom;
    s.trajectory.closed = false;
    s.trajectory.generator = GeneratorType::Wave;
    s.trajectory.genParams = {1, 35, 3, -6, 0, 0};
    s.trajectory.nodes = {
        Node{{1, 0, 0}, {0.9, 0.1, 0}, {0.9, -0.1, 0}, true},
        Node{{0, 1, 0}, {0.1, 0.9, 0}, {-0.1, 0.9, 0}, false},
    };

    s.matrix = {
        MatrixCell{MatrixTab::Features, 0, EncoderParam::MotionSpeed, 0.5},
        MatrixCell{MatrixTab::Generators, 2, EncoderParam::TransformYaw, -0.25},
    };

    s.sources[1].input = SourceInput::Sidechain;
    s.sources[4].input = SourceInput::Link;
    s.sources[4].link = Uuid::generate();

    s.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] = 42.0f;
    s.params[static_cast<std::size_t>(EncoderParam::MotionSpeed)] = -90.0f;
    return s;
}

}  // namespace

TEST_CASE("a fresh state carries the manifest defaults") {
    const PluginState s{encodeParams()};
    for (const auto& p : parameters()) {
        INFO(std::string(p.key));
        CHECK(s.params[static_cast<std::size_t>(p.id)] == doctest::Approx(p.def));
    }
}

TEST_CASE("round trip preserves everything") {
    const PluginState a = populated();
    PluginState b{encodeParams()};
    const LoadResult r = loadState(Product::Encoder, encodeParams(), saveState(Product::Encoder, encodeParams(), a), b);

    REQUIRE(r.ok);
    CHECK(r.fromVersion == kEncoderStateVersion);
    CHECK_FALSE(r.fromFuture);

    CHECK(b.identity.session == a.identity.session);
    CHECK(b.identity.instance == a.identity.instance);
    CHECK(b.identity.label == "rhodes");
    CHECK(b.identity.colour == 3);

    CHECK(b.trajectory.kind == TrajectoryKind::Custom);
    CHECK(b.trajectory.closed == false);
    CHECK(b.trajectory.generator == GeneratorType::Wave);
    for (int i = 0; i < 4; ++i) CHECK(b.trajectory.genParams[i] == doctest::Approx(a.trajectory.genParams[i]));

    REQUIRE(b.trajectory.nodes.size() == 2);
    CHECK(b.trajectory.nodes[0].p.x == doctest::Approx(1.0));
    CHECK(b.trajectory.nodes[1].smooth == false);
    CHECK(b.trajectory.nodes[0].cin.y == doctest::Approx(0.1));

    REQUIRE(b.matrix.size() == 2);
    CHECK(b.matrix[0].target == EncoderParam::MotionSpeed);
    CHECK(b.matrix[0].depth == doctest::Approx(0.5));
    CHECK(b.matrix[1].tab == MatrixTab::Generators);
    CHECK(b.matrix[1].source == 2);
    CHECK(b.matrix[1].depth == doctest::Approx(-0.25));

    CHECK(b.sources[1].input == SourceInput::Sidechain);
    CHECK(b.sources[4].input == SourceInput::Link);
    CHECK(b.sources[4].link == a.sources[4].link);
    CHECK(b.sources[0].input == SourceInput::Self);

    for (const auto& p : parameters()) {
        INFO(std::string(p.key));
        CHECK(b.params[static_cast<std::size_t>(p.id)] == doctest::Approx(a.params[static_cast<std::size_t>(p.id)]));
    }
}

TEST_CASE("round trip is stable — saving a loaded state reproduces the text exactly") {
    const std::string first = saveState(Product::Encoder, encodeParams(), populated());
    PluginState b{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), first, b).ok);
    CHECK(saveState(Product::Encoder, encodeParams(), b) == first);
}

/*  These four are the reason parameters serialise by key. Together they mean that adding or
 *  removing a parameter is not a schema change and needs no migration code. */
TEST_CASE("tolerant by construction: read by name, so growth needs no code") {
    SUBCASE("an unknown parameter key is ignored, not fatal") {
        PluginState s{encodeParams()};
        std::string text = saveState(Product::Encoder, encodeParams(), s);
        const auto at = text.find("\"input.trim\"");
        REQUIRE(at != std::string::npos);
        text.insert(at, "\"future.parameter\": 123.0,\n    ");

        PluginState out{encodeParams()};
        const LoadResult r = loadState(Product::Encoder, encodeParams(), text, out);
        CHECK(r.ok);
        CHECK(out.params[static_cast<std::size_t>(EncoderParam::InputTrim)] == doctest::Approx(0.0f));
    }

    SUBCASE("a missing parameter key takes the manifest default") {
        PluginState s{encodeParams()};
        s.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] = 90.0f;
        std::string text = saveState(Product::Encoder, encodeParams(), s);
        const auto at = text.find("\"render.width\"");
        REQUIRE(at != std::string::npos);
        const auto end = text.find('\n', at);
        text.erase(at, end - at + 1);

        PluginState out{encodeParams()};
        REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
        CHECK(out.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] ==
              doctest::Approx(parameter(EncoderParam::RenderWidth).def));
    }

    SUBCASE("a matrix cell aimed at an unknown parameter is dropped, not guessed") {
        PluginState s = populated();
        std::string text = saveState(Product::Encoder, encodeParams(), s);
        const auto at = text.find("\"motion.speed\"");
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string("\"motion.speed\"").size(), "\"gone.forever\"");

        PluginState out{encodeParams()};
        REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
        CHECK(out.matrix.size() == 1);  // the other cell survives
    }

    SUBCASE("an unknown enum name falls back rather than failing the load") {
        PluginState s{encodeParams()};
        //  a trajectory that is not a default, so the section is written at all
        s.trajectory.closed = false;
        std::string text = saveState(Product::Encoder, encodeParams(), s);
        const auto at = text.find("\"orbit\"");
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string("\"orbit\"").size(), "\"hypertrochoid\"");

        PluginState out{encodeParams()};
        const LoadResult r = loadState(Product::Encoder, encodeParams(), text, out);
        CHECK(r.ok);
        CHECK(out.trajectory.generator == GeneratorType::Orbit);  // the default a new encoder has
    }
}

TEST_CASE("out-of-range values are clamped on load, never propagated") {
    PluginState s{encodeParams()};
    std::string text = saveState(Product::Encoder, encodeParams(), s);
    const auto at = text.find("\"render.width\": 0");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string("\"render.width\": 0").size(), "\"render.width\": 9999");

    PluginState out{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
    CHECK(out.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] ==
          doctest::Approx(parameter(EncoderParam::RenderWidth).max));
}

TEST_CASE("version handling") {
    SUBCASE("a document from the future loads best-effort and says so") {
        PluginState s{encodeParams()};
        s.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] = 42.0f;
        std::string text = saveState(Product::Encoder, encodeParams(), s);
        const std::string current = "\"version\": " + std::to_string(kEncoderStateVersion);
        const auto at = text.find(current);
        REQUIRE(at != std::string::npos);
        text.replace(at, current.size(), "\"version\": 99");

        PluginState out{encodeParams()};
        const LoadResult r = loadState(Product::Encoder, encodeParams(), text, out);
        CHECK(r.ok);  // refusing would discard the user's work over fields we ignore
        CHECK(r.fromFuture);
        CHECK(r.fromVersion == 99);
        CHECK_FALSE(r.message.empty());
        CHECK(out.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] == doctest::Approx(42.0f));
    }

    SUBCASE("a missing or nonsensical version is refused") {
        PluginState out{encodeParams()};
        CHECK_FALSE(loadState(Product::Encoder, encodeParams(), "{}", out).ok);
        CHECK_FALSE(loadState(Product::Encoder, encodeParams(), R"({"version": 0})", out).ok);
        CHECK_FALSE(loadState(Product::Encoder, encodeParams(), R"({"version": "one"})", out).ok);
    }

    SUBCASE("malformed text is refused with a message") {
        PluginState out{encodeParams()};
        const LoadResult r = loadState(Product::Encoder, encodeParams(), "{ not json", out);
        CHECK_FALSE(r.ok);
        CHECK(r.message.find("malformed") != std::string::npos);
    }
}

TEST_CASE("generator parameters serialise by name, so array order cannot silently shift") {
    PluginState s{encodeParams()};
    s.trajectory.generator = GeneratorType::Arc;
    s.trajectory.genParams = {10, 20, 30, 40, 0, 0};
    const std::string text = saveState(Product::Encoder, encodeParams(), s);
    CHECK(text.find("\"centre_az\"") != std::string::npos);
    CHECK(text.find("\"heading\"") != std::string::npos);

    PluginState out{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
    CHECK(out.trajectory.genParams[0] == doctest::Approx(10));
    CHECK(out.trajectory.genParams[3] == doctest::Approx(40));
}

TEST_CASE("every generator declares no more names than the array can hold") {
    for (auto g : {GeneratorType::Orbit, GeneratorType::Lissajous, GeneratorType::Wave, GeneratorType::Arc,
                   GeneratorType::Spiral}) {
        INFO(std::string(name(g)));
        CHECK(generatorParamNames(g).size() <= kMaxGenParams);
        CHECK_FALSE(generatorParamNames(g).empty());
    }
}

TEST_CASE("sources serialise sparsely — defaults do not appear") {
    PluginState s{encodeParams()};
    const std::string plain = saveState(Product::Encoder, encodeParams(), s);
    CHECK(plain.find("\"sources\": {}") != std::string::npos);

    s.sources[4].input = SourceInput::Link;
    s.sources[4].link = Uuid::generate();
    const std::string text = saveState(Product::Encoder, encodeParams(), s);
    CHECK(text.find("\"4\"") != std::string::npos);
    CHECK(text.find("\"0\"") == std::string::npos);  // untouched slots stay absent

    PluginState out{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
    CHECK(out.sources[4].input == SourceInput::Link);
    CHECK(out.sources[4].link == s.sources[4].link);
    CHECK(out.sources[0].input == SourceInput::Self);
}

TEST_CASE("a source index this build does not have is ignored") {
    PluginState s{encodeParams()};
    s.sources[2].input = SourceInput::Sidechain;
    std::string text = saveState(Product::Encoder, encodeParams(), s);
    const auto at = text.find("\"sources\": {");
    REQUIRE(at != std::string::npos);
    text.insert(at + std::string("\"sources\": {").size(), "\n    \"999\": { \"input\": \"link\" },");

    PluginState out{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, out).ok);
    CHECK(out.sources[2].input == SourceInput::Sidechain);
}

TEST_CASE("envelope triggers round trip, and sparsely") {
    PluginState a{encodeParams()};
    a.envTriggers[1].noteLow = 48;
    a.envTriggers[1].noteHigh = 60;
    a.envTriggers[1].channel = 10;
    a.envTriggers[1].gate = TriggerGate::Held;
    a.envTriggers[1].velocity = 0.75;
    a.envTriggers[2].input = TriggerInput::Audio;
    a.envTriggers[2].source = 4;  // self Mid
    a.envTriggers[2].threshold = 0.625;
    a.envTriggers[2].hysteresis = 0.0625;

    const std::string text = saveState(Product::Encoder, encodeParams(), a);
    const Json j = Json::parse(text);
    const Json* envs = j.find("env_triggers");
    REQUIRE(envs != nullptr);
    CHECK(envs->size() == 2);  // the untouched one is at its default and must not appear

    PluginState b{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, b).ok);
    CHECK(b.envTriggers[0] == EnvTrigger::defaults(0));
    CHECK(b.envTriggers[1] == a.envTriggers[1]);
    CHECK(b.envTriggers[2] == a.envTriggers[2]);
}

TEST_CASE("a fresh instance's envelopes fire from MIDI, on the General MIDI drum map") {
    PluginState s{encodeParams()};
    CHECK(s.envTriggers[0].input == TriggerInput::Midi);
    CHECK(s.envTriggers[0].noteLow == 36);  // C1, kick
    CHECK(s.envTriggers[1].noteLow == 38);  // D1, snare
    CHECK(s.envTriggers[2].noteLow == 42);  // F#1, closed hi-hat
    for (const auto& t : s.envTriggers) {
        CHECK(t.noteLow == t.noteHigh);
        CHECK(t.channel == 0);
        CHECK(t.gate == TriggerGate::OneShot);
    }
}

TEST_CASE("an envelope trigger out of range is clamped on load, and a reversed range put in order") {
    const std::string text = R"({"version":1,"product":"encoder", "env_triggers": { "0": {
      "input": "midi", "note_low": 140, "note_high": -3, "channel": 40, "velocity": 7 } } })";
    PluginState s{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, s).ok);
    const auto& t = s.envTriggers[0];
    CHECK(t.noteLow == 0);
    CHECK(t.noteHigh == 127);
    CHECK(t.channel == 16);
    CHECK(t.velocity == doctest::Approx(1.0));
}

/*  A document with more nodes than the cap loads, refit rather than cut: truncating a loop keeps
 *  its first 24 nodes and closes it with one long segment across the rest of its shape. */
TEST_CASE("loading more nodes than the cap refits the curve instead of cutting it") {
    PluginState a{encodeParams()};
    a.trajectory.kind = TrajectoryKind::Custom;
    a.trajectory.closed = true;
    a.trajectory.nodes.clear();

    //  Built directly, around the insertion API that would refuse it: 36 nodes on a three-lobe
    //  wave around the equator, each handle a third of the way toward its neighbour.
    constexpr int kOver = 36;
    const double amp = 20.0 * kDeg2Rad;
    const double step = 2.0 * kPi / kOver;
    const auto wave = [&](double az) { return fromAzEl(az, amp * std::sin(3.0 * az)); };
    for (int i = 0; i < kOver; ++i) {
        Node n;
        n.p = wave(i * step);
        n.cin = wave(i * step - step / 3.0);
        n.cout = wave(i * step + step / 3.0);
        a.trajectory.nodes.push_back(n);
    }

    PluginState b{encodeParams()};
    const LoadResult r = loadState(Product::Encoder, encodeParams(), saveState(Product::Encoder, encodeParams(), a), b);
    REQUIRE(r.ok);
    INFO("load message: ", r.message);
    CHECK(b.trajectory.nodes.size() <= static_cast<std::size_t>(kMaxNodes));
    CHECK(b.trajectory.nodes.size() >= 4);
    CHECK(r.message.find("refit") != std::string::npos);  // and it says so

    //  The shape survived in both directions: nothing of the loaded curve strays from the
    //  original, and nothing of the original is missing from it.
    std::vector<Vec3> orig, loaded;
    for (const auto& x : samplePath(a.trajectory)) orig.push_back(x.p);
    for (const auto& x : samplePath(b.trajectory)) loaded.push_back(x.p);
    const auto directed = [](const std::vector<Vec3>& from, const std::vector<Vec3>& to) {
        double worst = 0.0;
        for (const Vec3& p : from) {
            double best = 1e9;
            for (const Vec3& q : to) best = std::min(best, arc(p, q));
            worst = std::max(worst, best);
        }
        return worst;
    };
    const double deviationDeg = std::max(directed(loaded, orig), directed(orig, loaded)) * kRad2Deg;
    INFO("deviation after refit: ", deviationDeg, " deg");
    CHECK(deviationDeg < 2.0);
}

TEST_CASE("a region's shape is saved by name; its side is a parameter and is not in the definition") {
    PluginState a{encodeParams()};
    a.regions[0].shape = {RegionKind::Sectors, 5, 12};
    a.params[static_cast<std::size_t>(EncoderParam::Region1Side)] = 1.0f;  // outside
    const std::string text = saveState(Product::Encoder, encodeParams(), a);
    PluginState b{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, b).ok);
    CHECK(b.regions[0] == a.regions[0]);

    /*  Enums as strings, never ordinals; and the side is not in the definition, so that copying
        "regions.region1" copies what the region is and nothing about how it is set. It is a
        parameter, so it rides in `parameters` with the rest of the block and there is no
        `region_uses` section at all. */
    const Json doc = Json::parse(text);
    const Json* def = doc.find("regions")->find("region1");
    REQUIRE(def != nullptr);
    CHECK(def->find("kind")->stringOr("") == "sectors");
    CHECK(def->find("side") == nullptr);
    CHECK(doc.find("region_uses") == nullptr);
    CHECK(doc.find("parameters")->find("region1.side")->numberOr(-1.0) == 1.0);

    //  sparse: a default region writes nothing, and there is no uses section to be empty
    const Json fresh = Json::parse(saveState(Product::Encoder, encodeParams(), PluginState{encodeParams()}));
    CHECK(fresh.find("regions")->members().empty());
    CHECK(fresh.find("region_uses") == nullptr);
}

TEST_CASE("a document without regions, or with one that cannot be, loads with a region that can") {
    //  an older document: no sections at all
    PluginState old{encodeParams()};
    old.regions[0].shape.kind = RegionKind::Dots;  // what was there before the load
    REQUIRE(loadState(Product::Encoder, encodeParams(), R"({"version":1,"product":"encoder"})", old).ok);
    CHECK(old.regions[0] == RegionEntry{});

    //  a kind from a newer build, no sectors, a dot count that is no solid, a region this build
    //  does not have
    PluginState s{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), R"({"version":1,"product":"encoder",
        "regions":{"region1":{"kind":"blobs","sectors":0,"dots":7},"region9":{"kind":"band"}},
        "region_uses":{"region1":{"side":"sideways"}}})",
                      s)
                .ok);
    CHECK(s.regions[0].shape.kind == RegionShape{}.kind);
    CHECK(s.regions[0].shape.sectors == 1);
    CHECK(s.regions[0].shape.dots == 6);
    CHECK(s.params[static_cast<std::size_t>(EncoderParam::Region1Side)] == 0.0f);  // "sideways" is not a side
}

TEST_CASE("a cell on the region's tab is saved by name; one on a tab this build lacks is dropped") {
    PluginState a{encodeParams()};
    a.matrix.push_back(MatrixCell{MatrixTab::Region, 0, EncoderParam::MotionSpeed, 0.4});
    const std::string text = saveState(Product::Encoder, encodeParams(), a);
    CHECK(text.find("\"region\"") != std::string::npos);
    PluginState b{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, b).ok);
    REQUIRE(b.matrix.size() == 1);
    CHECK(b.matrix[0].tab == MatrixTab::Region);

    //  Read as the first tab, a newer build's source would arrive here as level driving the target.
    PluginState c{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), R"({"version":1,"product":"encoder","matrix":[
        {"tab":"weather","source":0,"target":"motion.speed","depth":0.5},
        {"tab":"features","source":1,"target":"motion.speed","depth":0.25}]})",
                      c)
                .ok);
    REQUIRE(c.matrix.size() == 1);
    CHECK(c.matrix[0].source == 1);
}

TEST_CASE("a slot key that is not a whole number is ignored, not read as slot 0") {
    //  Catches: a lenient parse that reads "1x" as 1 (or "x" as 0) and loads a stray entry into a slot.
    PluginState a{encodeParams()};
    a.sources[1].input = SourceInput::Sidechain;
    std::string text = saveState(Product::Encoder, encodeParams(), a);
    const auto at = text.find("\"1\"", text.find("\"sources\""));
    REQUIRE(at != std::string::npos);
    text.replace(at, 3, "\"1x\"");
    PluginState b{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, b).ok);
    CHECK(b.sources[1].input == SourceInput::Self);
}
