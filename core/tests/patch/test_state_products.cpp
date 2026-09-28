// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  A document belongs to a plugin. One state type serves all three -- a plugin owns its engine and
 *  its parameter set, and a third owned thing is the signal to look again -- but
 *  a saved one is its writer's, and must say so.
 */

TEST_CASE("a document says which plugin wrote it, and another plugin refuses it whole") {
    /*  Catches: the product check dropped. Every key both plugins share would land and every key
        only the writer has would not, leaving half a patch -- and the user would see a reverb that
        had quietly taken an echo's LFOs. Refusing is the only honest answer.

        The check is asserted both ways, so it cannot be a rule about who is senior. */
    PluginState echoState{echoParams()};
    echoState.identity.label = "tape";
    echoState.params[static_cast<std::size_t>(echoParams().byKey("tap1.level"))] = -12.0f;
    const std::string doc = saveState(Product::Echo, echoParams(), echoState);
    CHECK(doc.find("\"product\": \"echo\"") != std::string::npos);

    //  into Reverb: refused, and nothing is touched
    PluginState reverbState{reverbParams()};
    const PluginState before = reverbState;
    const LoadResult bad = loadState(Product::Reverb, reverbParams(), doc, reverbState);
    CHECK_FALSE(bad.ok);
    CHECK(bad.fromWrongProduct);
    CHECK(bad.product == Product::Echo);
    CHECK(bad.message.find("echo") != std::string::npos);
    CHECK(reverbState.identity.label == before.identity.label);
    for (std::size_t i = 0; i < reverbState.params.size(); ++i) REQUIRE(reverbState.params[i] == before.params[i]);

    //  and the other way round
    const std::string rdoc = saveState(Product::Reverb, reverbParams(), PluginState{reverbParams()});
    PluginState intoEcho{echoParams()};
    CHECK_FALSE(loadState(Product::Echo, echoParams(), rdoc, intoEcho).ok);

    //  into its own plugin it loads, of course
    PluginState back{echoParams()};
    const LoadResult good = loadState(Product::Echo, echoParams(), doc, back);
    CHECK(good.ok);
    CHECK_FALSE(good.fromWrongProduct);
    CHECK(back.identity.label == "tape");
    CHECK(back.params[static_cast<std::size_t>(echoParams().byKey("tap1.level"))] == -12.0f);
}

TEST_CASE("a document that does not say which plugin wrote it is refused") {
    /*  Nothing has shipped, so there is no document "from before products were named" to be kind
        to. Catches: absence read as this plugin's own -- any JSON with a version then loads
        into whichever plugin is handed it. */
    PluginState s{encodeParams()};
    const LoadResult r =
        loadState(Product::Encoder, encodeParams(), R"({"version":1,"identity":{"label":"whose"}})", s);
    CHECK_FALSE(r.ok);
    CHECK(r.fromWrongProduct);
    CHECK(s.identity.label.empty());
}

TEST_CASE("a parameter means what it says in every plugin's document: nothing is rewritten on load") {
    /*  There are no migrations: a value is read and that is all. Catches: any code that
        rewrites a loaded value by the version that wrote it. */
    const int div = 3;
    for (const Product p : {Product::Encoder, Product::Echo}) {
        const ParamManifest& m = p == Product::Encoder ? encodeParams() : echoParams();
        const std::string doc = std::string(R"({"version":1,"product":")") + std::string(name(p)) +
                                R"(","parameters":{"lfo1.div":)" + std::to_string(div) + "}}";
        PluginState st{m};
        REQUIRE(loadState(p, m, doc, st).ok);
        CHECK(st.params[static_cast<std::size_t>(m.byKey("lfo1.div"))] == doctest::Approx(div));
    }
    //  and every product writes version 1 until there is a release to count from
    CHECK(stateVersionFor(Product::Encoder) == 1);
    CHECK(stateVersionFor(Product::Echo) == 1);
    CHECK(stateVersionFor(Product::Reverb) == 1);
}

TEST_CASE("Reverb has two region slots and they save and load apart") {
    /*  The count is the manifest's, from the keys themselves, so it cannot disagree with the list.
        Catches: the count fixed at one, and the return's shape is silently lost. */
    CHECK(encodeParams().regionCount() == 1);
    CHECK(echoParams().regionCount() == 1);
    CHECK(reverbParams().regionCount() == 2);

    PluginState s{reverbParams()};
    s.regions[0].shape = {RegionKind::Sectors, 5, 6};  // the send
    s.regions[1].shape = {RegionKind::Dots, 4, 12};    // the return
    PluginState back{reverbParams()};
    REQUIRE(loadState(Product::Reverb, reverbParams(), saveState(Product::Reverb, reverbParams(), s), back).ok);
    CHECK(back.regions[0] == s.regions[0]);
    CHECK(back.regions[1] == s.regions[1]);
    CHECK(back.regions[0].shape.kind == RegionKind::Sectors);
    CHECK(back.regions[1].shape.kind == RegionKind::Dots);
    CHECK(back.regions[1].shape.dots == 12);

    /*  And a slot a plugin does not have is not stored: Echo has one, so a document naming a second
        is a slot it has no parameters for, which could be neither set nor heard. Catches: the bound
        taken from the array's size rather than the manifest's count. */
    PluginState intoEcho{echoParams()};
    REQUIRE(loadState(Product::Echo, echoParams(),
                      R"({"version":1,"product":"echo","regions":{"region1":{"kind":"band"},
                          "region2":{"kind":"dots"}}})",
                      intoEcho)
                .ok);
    CHECK(intoEcho.regions[0].shape.kind == RegionKind::Band);
    CHECK(intoEcho.regions[1] == RegionEntry{});  // untouched
    CHECK(saveState(Product::Echo, echoParams(), intoEcho).find("region2") == std::string::npos);
}

TEST_CASE("an effect's document carries no trajectory") {
    //  The trajectory is the encoder's alone, and is written only when it is not a default.
    //  Catches: written always, and every effect's patch carries a section that means nothing in it.
    const std::string echoDoc = saveState(Product::Echo, echoParams(), PluginState{echoParams()});
    CHECK(echoDoc.find("trajectory") == std::string::npos);

    //  but the encoder's, once it has one, does
    PluginState enc{encodeParams()};
    enc.trajectory.kind = TrajectoryKind::Custom;
    CHECK(saveState(Product::Encoder, encodeParams(), enc).find("trajectory") != std::string::npos);
}

TEST_CASE("where a starting point's values came from is saved with the patch") {
    const ParamManifest& rm = reverbParams();
    const ParamManifest& em = echoParams();

    SUBCASE("the room and its shape round-trip by name") {
        //  Mutation: drop the write or the read of either field and it comes back as hall.
        PluginState out{rm};
        out.room = {5, 2};  // cathedral, tall
        const auto text = saveState(Product::Reverb, rm, out);
        CHECK(text.find("\"cathedral\"") != std::string::npos);
        CHECK(text.find("\"tall\"") != std::string::npos);
        PluginState back{rm};
        REQUIRE(loadState(Product::Reverb, rm, text, back).ok);
        CHECK(back.room == out.room);
    }

    SUBCASE("the pattern round-trips by name") {
        PluginState out{em};
        out.echoPattern = 1;  // ping-pong
        const auto text = saveState(Product::Echo, em, out);
        CHECK(text.find("\"ping-pong\"") != std::string::npos);
        PluginState back{em};
        REQUIRE(loadState(Product::Echo, em, text, back).ok);
        CHECK(back.echoPattern == 1);
    }

    SUBCASE("the defaults are not written, and an unknown name loads as the default") {
        //  Sparse: the encoder's document carries neither. Catches: write unconditionally.
        PluginState enc{encodeParams()};
        const auto text = saveState(Product::Encoder, encodeParams(), enc);
        CHECK(text.find("\"room\"") == std::string::npos);
        CHECK(text.find("\"pattern\"") == std::string::npos);
        PluginState back{rm};
        back.room = {0, 0};
        REQUIRE(loadState(Product::Reverb, rm,
                          "{\"product\":\"reverb\",\"version\":1,\"room\":{\"preset\":\"cave\",\"shape\":\"dome\"}}",
                          back)
                    .ok);
        CHECK(back.room == RoomState{});
    }
}

TEST_CASE("what a bounce should do is saved with the patch") {
    const ParamManifest& m = reverbParams();

    SUBCASE("it round-trips") {
        //  Mutation: drop the write, or the read, and it comes back Realistic.
        PluginState out{m};
        out.renderQuality = RenderQuality::Same;
        PluginState back{m};
        REQUIRE(loadState(Product::Reverb, m, saveState(Product::Reverb, m, out), back).ok);
        CHECK(back.renderQuality == RenderQuality::Same);
    }

    SUBCASE("the default is not written at all") {
        /*  Sparse, as the trajectory and the regions are: a plugin with no quality switch
            carries no key for one. Catches: write it unconditionally and this fails. */
        PluginState out{m};
        CHECK(out.renderQuality == RenderQuality::Realistic);
        CHECK(saveState(Product::Reverb, m, out).find("render_quality") == std::string::npos);

        PluginState echoState{echoParams()};
        CHECK(saveState(Product::Echo, echoParams(), echoState).find("render_quality") == std::string::npos);
    }

    SUBCASE("a document written before it existed reads as the default") {
        //  Growth of this kind needs no version bump -- the point of serialising by name. An
        //  existing v1 effect document simply has no such key.
        PluginState out{m};
        const std::string old = saveState(Product::Reverb, m, out);
        REQUIRE(old.find("render_quality") == std::string::npos);
        PluginState back{m};
        back.renderQuality = RenderQuality::Same;  // whatever it held before the load
        REQUIRE(loadState(Product::Reverb, m, old, back).ok);
        CHECK(back.renderQuality == RenderQuality::Realistic);
    }

    SUBCASE("a name this build does not know falls back rather than failing") {
        PluginState out{m};
        out.renderQuality = RenderQuality::Same;
        std::string text = saveState(Product::Reverb, m, out);
        const auto at = text.find("\"same\"");
        REQUIRE(at != std::string::npos);
        text.replace(at, 6, "\"sublime\"");
        PluginState back{m};
        REQUIRE(loadState(Product::Reverb, m, text, back).ok);
        CHECK(back.renderQuality == RenderQuality::Realistic);
    }
}
