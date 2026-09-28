// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "bambi/encode/params.hpp"
#include "bambi/patch/undo.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
float width(const PluginState& s) { return s.params[static_cast<std::size_t>(EncoderParam::RenderWidth)]; }
void setWidth(PluginState& s, float v) { s.params[static_cast<std::size_t>(EncoderParam::RenderWidth)] = v; }
}  // namespace

TEST_CASE("a fresh stack has nothing to undo") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    CHECK_FALSE(u.canUndo());
    CHECK_FALSE(u.canRedo());
    CHECK(u.undoName().empty());
    CHECK_FALSE(u.undo());
    CHECK_FALSE(u.redo());
}

TEST_CASE("undo and redo restore exactly") {
    PluginState s{encodeParams()};
    UndoStack u(s);

    u.perform("set width", [](PluginState& st) { setWidth(st, 42.0f); });
    CHECK(width(s) == doctest::Approx(42.0f));
    CHECK(u.canUndo());
    CHECK(u.undoName() == "set width");

    REQUIRE(u.undo());
    CHECK(width(s) == doctest::Approx(parameter(EncoderParam::RenderWidth).def));
    CHECK_FALSE(u.canUndo());
    CHECK(u.canRedo());
    CHECK(u.redoName() == "set width");

    REQUIRE(u.redo());
    CHECK(width(s) == doctest::Approx(42.0f));
}

TEST_CASE("the destructive operations are what this exists for") {
    // "I spent ten minutes shaping that and hit generate."
    PluginState s{encodeParams()};
    s.trajectory.kind = TrajectoryKind::Custom;
    s.trajectory.nodes.resize(14);

    UndoStack u(s);
    u.perform("generate", [](PluginState& st) {
        st.trajectory.nodes.clear();
        st.trajectory.kind = TrajectoryKind::Parametric;
    });
    CHECK(s.trajectory.nodes.empty());

    REQUIRE(u.undo());
    CHECK(s.trajectory.nodes.size() == 14);
    CHECK(s.trajectory.kind == TrajectoryKind::Custom);
}

TEST_CASE("a new edit drops the redo branch") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.perform("a", [](PluginState& st) { setWidth(st, 10.0f); });
    u.perform("b", [](PluginState& st) { setWidth(st, 20.0f); });
    REQUIRE(u.undo());
    CHECK(u.canRedo());

    u.perform("c", [](PluginState& st) { setWidth(st, 30.0f); });
    CHECK_FALSE(u.canRedo());
    CHECK(width(s) == doctest::Approx(30.0f));

    REQUIRE(u.undo());
    CHECK(width(s) == doctest::Approx(10.0f));
}

TEST_CASE("coalescing turns a drag into one undo step") {
    PluginState s{encodeParams()};
    UndoStack u(s);

    for (int i = 1; i <= 200; ++i)
        u.performCoalescing("move node", "node.3", [i](PluginState& st) { setWidth(st, static_cast<float>(i)); });
    u.endGesture();

    CHECK(u.depth() == 1);
    CHECK(width(s) == doctest::Approx(200.0f));

    REQUIRE(u.undo());
    CHECK(width(s) == doctest::Approx(parameter(EncoderParam::RenderWidth).def));
    CHECK_FALSE(u.canUndo());
}

TEST_CASE("separate gestures stay separate") {
    PluginState s{encodeParams()};
    UndoStack u(s);

    u.performCoalescing("move node", "node.3", [](PluginState& st) { setWidth(st, 10.0f); });
    u.performCoalescing("move node", "node.3", [](PluginState& st) { setWidth(st, 20.0f); });
    u.endGesture();
    u.performCoalescing("move node", "node.3", [](PluginState& st) { setWidth(st, 30.0f); });
    u.endGesture();

    CHECK(u.depth() == 2);
    REQUIRE(u.undo());
    CHECK(width(s) == doctest::Approx(20.0f));
    REQUIRE(u.undo());
    CHECK(width(s) == doctest::Approx(parameter(EncoderParam::RenderWidth).def));
}

TEST_CASE("a different key starts a new step even without endGesture") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.performCoalescing("move node 3", "node.3", [](PluginState& st) { setWidth(st, 10.0f); });
    u.performCoalescing("move node 7", "node.7", [](PluginState& st) { setWidth(st, 20.0f); });
    CHECK(u.depth() == 2);
}

TEST_CASE("an empty key never coalesces") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    for (int i = 0; i < 5; ++i)
        u.performCoalescing("edit", "", [i](PluginState& st) { setWidth(st, static_cast<float>(i)); });
    CHECK(u.depth() == 5);
}

TEST_CASE("perform interrupts a coalescing run") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.performCoalescing("drag", "k", [](PluginState& st) { setWidth(st, 1.0f); });
    u.perform("other", [](PluginState& st) { setWidth(st, 2.0f); });
    u.performCoalescing("drag", "k", [](PluginState& st) { setWidth(st, 3.0f); });
    CHECK(u.depth() == 3);
}

TEST_CASE("the stack is bounded and drops the oldest") {
    PluginState s{encodeParams()};
    UndoStack u(s, 8);
    for (int i = 1; i <= 50; ++i) u.perform("edit", [i](PluginState& st) { setWidth(st, static_cast<float>(i)); });

    CHECK(u.depth() == 8);
    for (int i = 0; i < 8; ++i) CHECK(u.undo());
    CHECK_FALSE(u.canUndo());
    // Oldest reachable state is the one before edit 43, i.e. width 42.
    CHECK(width(s) == doctest::Approx(42.0f));
}

TEST_CASE("clear forgets everything — for a preset load") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.perform("edit", [](PluginState& st) { setWidth(st, 5.0f); });
    REQUIRE(u.undo());
    REQUIRE(u.canRedo());
    u.clear();
    CHECK_FALSE(u.canUndo());
    CHECK_FALSE(u.canRedo());
}

TEST_CASE("history survives a save/load round trip of the state it points at") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.perform("set width", [](PluginState& st) { setWidth(st, 77.0f); });

    const std::string text = saveState(Product::Encoder, encodeParams(), s);
    PluginState reloaded{encodeParams()};
    REQUIRE(loadState(Product::Encoder, encodeParams(), text, reloaded).ok);
    CHECK(width(reloaded) == doctest::Approx(77.0f));

    REQUIRE(u.undo());  // the stack still governs its own target
    CHECK(width(s) == doctest::Approx(parameter(EncoderParam::RenderWidth).def));
}

TEST_CASE("a whole step says so, undone and redone, and no other step does") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    u.perform("edit", [](PluginState& st) { setWidth(st, 10.0f); });
    CHECK_FALSE(u.undoIsWhole());
    u.performWhole("load preset", [](PluginState& st) { setWidth(st, 20.0f); });
    CHECK(u.undoIsWhole());
    CHECK_FALSE(u.redoIsWhole());

    REQUIRE(u.undo());
    CHECK(width(s) == 10.0f);
    CHECK(u.redoIsWhole());        // the step carried its mark across
    CHECK_FALSE(u.undoIsWhole());  // and what is under it never had one
    REQUIRE(u.redo());
    CHECK(width(s) == 20.0f);
    CHECK(u.undoIsWhole());
}

TEST_CASE("amending everywhere changes the patch and its history, and is no step") {
    PluginState s{encodeParams()};
    UndoStack u(s);
    s.preset = {false, "", "old"};
    u.perform("edit", [](PluginState& st) { setWidth(st, 10.0f); });
    u.perform("edit", [](PluginState& st) { setWidth(st, 20.0f); });
    REQUIRE(u.undo());
    const auto depth = u.depth();
    u.amendEverywhere([](PluginState& st) {
        if (st.preset.name == "old") st.preset.name = "new";
    });
    CHECK(u.depth() == depth);
    CHECK(s.preset.name == "new");
    CHECK(width(s) == 10.0f);
    REQUIRE(u.redo());
    CHECK(s.preset.name == "new");
    REQUIRE(u.undo());
    REQUIRE(u.undo());
    CHECK(s.preset.name == "new");
}
