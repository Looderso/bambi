// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <string>

#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/patch/presets.hpp"
#include "bambi/reverb/control.hpp"
#include "bambi/reverb/params.hpp"
#include "bambi/reverb/presets.hpp"
#include "doctest.h"

using namespace bambi;
namespace fs = std::filesystem;

namespace {
/// A directory of this test's own, gone when the test ends.
struct Scratch {
    fs::path dir;
    explicit Scratch(const char* name) : dir(fs::temp_directory_path() / "bambi-tests-presets" / name) {
        fs::remove_all(dir);
    }
    ~Scratch() { fs::remove_all(dir); }
};

std::size_t at(const ParamManifest& m, std::string_view key) { return static_cast<std::size_t>(m.byKey(key)); }

PresetLibrary echoLibrary(const fs::path& root) {
    return PresetLibrary(Product::Echo, echoParams(), PluginState{echoParams()}, {}, root);
}

std::vector<std::string> paths(const std::vector<PresetRef>& refs) {
    std::vector<std::string> out;
    for (const auto& r : refs) out.push_back((r.factory ? "f:" : "u:") + presetPath(r));
    return out;
}
}  // namespace

TEST_CASE("a typed name: a slash makes the folder, one level deep") {
    CHECK(parsePresetPath("tight room") == PresetRef{false, "", "tight room"});
    CHECK(parsePresetPath("drums/tight room") == PresetRef{false, "drums", "tight room"});
    CHECK(parsePresetPath("  drums /  tight room ") == PresetRef{false, "drums", "tight room"});
    CHECK(parsePresetPath("a/b/c") == PresetRef{false, "a", "b c"});  // what follows a second slash joins the name
    CHECK(parsePresetPath("/loose") == PresetRef{false, "", "loose"});
    CHECK(parsePresetPath("what?:*") == PresetRef{false, "", "what"});  // what a file name cannot hold is dropped
    CHECK_FALSE(parsePresetPath("").has_value());
    CHECK_FALSE(parsePresetPath(" / ").has_value());
    CHECK_FALSE(parsePresetPath("..").has_value());
    CHECK(presetPath(*parsePresetPath("drums/tight room")) == "drums/tight room");
}

TEST_CASE("a typed name cannot leave the preset directory") {
    //  ".." loses its dots, so no segment can climb
    const auto ref = parsePresetPath("../../etc/passwd");
    REQUIRE(ref.has_value());
    CHECK(ref->folder == "etc");
    CHECK(ref->name == "passwd");
    Scratch scratch("climb");
    auto lib = echoLibrary(scratch.dir);
    const auto file = lib.fileOf(*ref).lexically_normal().string();
    CHECK(file.starts_with(scratch.dir.lexically_normal().string()));
}

TEST_CASE("a preset's document has no identity, no preset name and none of the kept keys") {
    const auto& m = encodeParams();
    PluginState s{m};
    s.identity.label = "lead vocal";
    s.preset = {false, "drums", "x"};
    const auto text = presetText(Product::Encoder, m, s);
    CHECK(text.find("identity") == std::string::npos);
    CHECK(text.find("\"preset\"") == std::string::npos);
    CHECK(text.find("input.mode") == std::string::npos);
    CHECK(text.find("input.spread") != std::string::npos);  // its neighbours are the sound's

    //  and one that carries them anyway -- a session document dropped in the folder -- loads without them
    PluginState back{m};
    REQUIRE(loadPresetText(Product::Encoder, m, saveState(Product::Encoder, m, s), back).ok);
    CHECK(back.identity.label.empty());
    CHECK(back.preset.none());
}

TEST_CASE("a session's document carries the preset it came from; a patch from none says nothing") {
    const auto& m = echoParams();
    PluginState s{m};
    CHECK(saveState(Product::Echo, m, s).find("\"preset\"") == std::string::npos);
    s.preset = {false, "songs", "outro"};
    PluginState back{m};
    REQUIRE(loadState(Product::Echo, m, saveState(Product::Echo, m, s), back).ok);
    CHECK(back.preset == PresetRef{false, "songs", "outro"});
    s.preset = kDefaultPreset;
    REQUIRE(loadState(Product::Echo, m, saveState(Product::Echo, m, s), back).ok);
    CHECK(back.preset == kDefaultPreset);
}

TEST_CASE("adopting a preset keeps who the instance is and what is plugged in") {
    const auto& m = encodeParams();
    PluginState preset{m};
    preset.params[at(m, "render.width")] = 77.0f;
    preset.params[at(m, "input.mode")] = 2.0f;
    preset.matrix.push_back({MatrixTab::Features, 0, static_cast<ParamId>(at(m, "render.width")), 0.5});

    PluginState mine{m};
    mine.identity.label = "lead vocal";
    mine.params[at(m, "input.mode")] = 1.0f;
    adoptPreset(Product::Encoder, m, preset, {false, "", "wide"}, mine);

    CHECK(mine.identity.label == "lead vocal");
    CHECK(mine.params[at(m, "input.mode")] == 1.0f);
    CHECK(mine.params[at(m, "render.width")] == 77.0f);
    CHECK(mine.matrix.size() == 1);
    CHECK(mine.preset == PresetRef{false, "", "wide"});
}

TEST_CASE("the same sound: everything a preset carries, within what a host parameter's round trip costs") {
    const auto& m = encodeParams();
    PluginState a{m}, b{m};
    CHECK(sameSound(Product::Encoder, m, a, b));

    b.identity.label = "another";
    b.preset = {false, "", "x"};
    b.params[at(m, "input.mode")] = 2.0f;
    CHECK(sameSound(Product::Encoder, m, a, b));  // none of which is the sound

    const auto width = at(m, "render.width");
    const float range = m[static_cast<int>(width)].max - m[static_cast<int>(width)].min;
    b.params[width] = a.params[width] + 0.5e-4f * range;
    CHECK(sameSound(Product::Encoder, m, a, b));
    b.params[width] = a.params[width] + 3.0e-4f * range;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
    b.params[width] = a.params[width];

    b.matrix.push_back({MatrixTab::Features, 0, static_cast<ParamId>(width), 0.5});
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
    b.matrix.clear();
    b.trajectory.closed = !b.trajectory.closed;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
    b.trajectory = a.trajectory;
    b.trajectory.nodes.push_back({});
    a.trajectory.nodes.push_back({});
    b.trajectory.nodes[0].smooth = !a.trajectory.nodes[0].smooth;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));  // a node's every field, not only its point
    b.trajectory.nodes[0].smooth = a.trajectory.nodes[0].smooth;
    b.trajectory.nodes[0].cin.z += 0.5;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
    b.trajectory = a.trajectory;
    b.envTriggers[1].noteLow = 50;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
    b.envTriggers = a.envTriggers;
    b.regions[0].shape.sectors += 1;
    CHECK_FALSE(sameSound(Product::Encoder, m, a, b));
}

TEST_CASE("a library: save, list, load, in list order") {
    Scratch scratch("library");
    auto lib = echoLibrary(scratch.dir);
    CHECK(paths(lib.all()) == std::vector<std::string>{"f:default"});  // and the directory need not exist
    CHECK_FALSE(fs::exists(scratch.dir));

    const auto& m = echoParams();
    PluginState s{m};
    s.params[0] = m[0].max;
    REQUIRE(lib.save({false, "songs", "outro"}, s));
    REQUIRE(lib.save({false, "", "verse"}, s));
    REQUIRE(lib.save({false, "songs", "Bridge"}, s));
    REQUIRE(lib.save({false, "drums", "tight"}, s));
    REQUIRE(lib.save({false, "", "attic"}, s));
    REQUIRE(lib.save({false, "", "Zed"}, s));

    //  factory first; in each group the loose ones, then folder by folder; user names alphabetical whatever their case
    CHECK(paths(lib.all()) == std::vector<std::string>{"f:default", "u:attic", "u:verse", "u:Zed", "u:drums/tight",
                                                       "u:songs/Bridge", "u:songs/outro"});
    CHECK(lib.folders(false) == std::vector<std::string>{"drums", "songs"});
    CHECK(lib.folders(true).empty());

    const auto back = lib.load({false, "songs", "outro"});
    REQUIRE(back.has_value());
    CHECK(back->params[0] == m[0].max);
    CHECK(sameSound(Product::Echo, m, *back, s));
    CHECK_FALSE(lib.load({false, "songs", "nothing"}).has_value());

    //  a second library over the same directory finds them: they are files, not a session's
    auto again = echoLibrary(scratch.dir);
    CHECK(again.all() == lib.all());
}

TEST_CASE("a library refuses what is not a user preset's, and another plugin's file") {
    Scratch scratch("refuse");
    auto lib = echoLibrary(scratch.dir);
    PluginState s{echoParams()};
    CHECK_FALSE(lib.save(kDefaultPreset, s));
    CHECK_FALSE(lib.save({false, "", ""}, s));
    CHECK_FALSE(lib.remove(kDefaultPreset));
    CHECK_FALSE(lib.rename(kDefaultPreset, {false, "", "mine"}));

    fs::create_directories(scratch.dir);
    std::ofstream(scratch.dir / "stray.json")
        << presetText(Product::Reverb, reverbParams(), PluginState{reverbParams()});
    std::ofstream(scratch.dir / "broken.json") << "{ not json";
    std::ofstream(scratch.dir / ".hidden.json") << "{}";
    std::ofstream(scratch.dir / "notes.txt") << "x";
    lib.scan();
    CHECK(lib.all().size() == 3);          // default, stray, broken: not the hidden one, not the text file
    CHECK(lib.has({false, "", "stray"}));  // listed, since a name is all a scan reads
    CHECK_FALSE(lib.load({false, "", "stray"}).has_value());
    CHECK_FALSE(lib.load({false, "", "broken"}).has_value());
}

TEST_CASE("rename moves, refuses a name that is taken, and a folder left empty goes") {
    Scratch scratch("rename");
    auto lib = echoLibrary(scratch.dir);
    PluginState s{echoParams()};
    REQUIRE(lib.save({false, "drums", "tight"}, s));
    REQUIRE(lib.save({false, "", "verse"}, s));

    CHECK_FALSE(lib.rename({false, "drums", "tight"}, {false, "", "verse"}));
    CHECK(lib.has({false, "drums", "tight"}));

    REQUIRE(lib.rename({false, "drums", "tight"}, {false, "vocals", "tight"}));
    CHECK(paths(lib.all()) == std::vector<std::string>{"f:default", "u:verse", "u:vocals/tight"});
    CHECK_FALSE(fs::exists(scratch.dir / "drums"));

    //  a folder holding only what the Finder leaves behind is empty too
    std::ofstream(scratch.dir / "vocals" / ".DS_Store") << "x";
    REQUIRE(lib.remove({false, "vocals", "tight"}));
    CHECK_FALSE(fs::exists(scratch.dir / "vocals"));
    CHECK(paths(lib.all()) == std::vector<std::string>{"f:default", "u:verse"});
}

TEST_CASE("remove disposes of the file the way the library was told to, and keeps the preset when that fails") {
    Scratch scratch("remove");
    fs::path asked;
    bool let = false;
    PresetLibrary lib(Product::Echo, echoParams(), PluginState{echoParams()}, {}, scratch.dir,
                      [&](const fs::path& file) {
                          asked = file;
                          return let && fs::remove(file);
                      });
    REQUIRE(lib.save({false, "", "verse"}, PluginState{echoParams()}));

    CHECK_FALSE(lib.remove({false, "", "verse"}));
    CHECK(asked == lib.fileOf({false, "", "verse"}));
    CHECK(lib.has({false, "", "verse"}));

    let = true;
    CHECK(lib.remove({false, "", "verse"}));
    CHECK_FALSE(lib.has({false, "", "verse"}));
    CHECK_FALSE(fs::exists(asked));
}

TEST_CASE("modified: against the preset's file, read once; a file gone counts as moved; no preset never does") {
    Scratch scratch("modified");
    auto lib = echoLibrary(scratch.dir);
    const auto& m = echoParams();
    PluginState live{m};
    CHECK_FALSE(lib.modified(live));  // from no preset

    live.preset = kDefaultPreset;
    CHECK_FALSE(lib.modified(live));
    //  a key whose default is not its maximum, so that setting it there is a move
    int moved = 0;
    while (m[moved].def == m[moved].max) ++moved;
    live.params[static_cast<std::size_t>(moved)] = m[moved].max;
    CHECK(lib.modified(live));

    REQUIRE(lib.save({false, "", "mine"}, live));
    live.preset = {false, "", "mine"};
    CHECK_FALSE(lib.modified(live));
    live.matrix.push_back({MatrixTab::Features, 0, static_cast<ParamId>(0), 0.5});
    CHECK(lib.modified(live));
    live.matrix.clear();
    CHECK_FALSE(lib.modified(live));

    //  the file rewritten under the same name: a scan reads it again
    live.params[static_cast<std::size_t>(moved)] = m[moved].min;
    REQUIRE(lib.save({false, "", "mine"}, live));
    CHECK_FALSE(lib.modified(live));

    REQUIRE(lib.remove({false, "", "mine"}));
    CHECK(lib.modified(live));  // its file is gone: unsaved
}

TEST_CASE("search is over the whole path and ignores case; a step wraps and ignores the search") {
    Scratch scratch("search");
    auto lib = echoLibrary(scratch.dir);
    PluginState s{echoParams()};
    REQUIRE(lib.save({false, "Songs", "outro"}, s));
    REQUIRE(lib.save({false, "Songs", "bridge"}, s));
    REQUIRE(lib.save({false, "", "verse"}, s));

    CHECK(paths(searchPresets(lib.all(), " songs ")) == std::vector<std::string>{"u:Songs/bridge", "u:Songs/outro"});
    CHECK(paths(searchPresets(lib.all(), "RS")) == std::vector<std::string>{"u:verse"});
    CHECK(searchPresets(lib.all(), "").size() == lib.all().size());
    CHECK(searchPresets(lib.all(), "zzz").empty());

    const auto& all = lib.all();  // default, verse, Songs/bridge, Songs/outro
    CHECK(stepPreset(all, kDefaultPreset, 1) == PresetRef{false, "", "verse"});
    CHECK(stepPreset(all, kDefaultPreset, -1) == PresetRef{false, "Songs", "outro"});
    CHECK(stepPreset(all, {false, "Songs", "outro"}, 1) == kDefaultPreset);
    CHECK(stepPreset(all, {false, "", "gone"}, 1) == PresetRef{false, "", "verse"});
}

TEST_CASE("which headings are folded is the user's: a second library finds it") {
    Scratch scratch("folded");
    {
        auto lib = echoLibrary(scratch.dir);
        CHECK_FALSE(lib.folded("factory"));
        lib.setFolded("factory", true);
        lib.setFolded("user/songs", true);
        lib.setFolded("user/songs", false);
        CHECK(lib.folded("factory"));
    }
    auto again = echoLibrary(scratch.dir);
    CHECK(again.folded("factory"));
    CHECK_FALSE(again.folded("user/songs"));
    CHECK(paths(again.all()) == std::vector<std::string>{"f:default"});  // and its file is not a preset
}

TEST_CASE("default is the fresh patch the plugin hands over, not the manifest's") {
    Scratch scratch("default");
    const auto& m = encodeParams();
    PluginState fresh{m};
    fresh.trajectory.genParams[0] = 0.25;
    fresh.identity.label = "not part of it";
    PresetLibrary lib(Product::Encoder, m, fresh, {}, scratch.dir);
    const auto loaded = lib.load(kDefaultPreset);
    REQUIRE(loaded.has_value());
    CHECK(loaded->trajectory.genParams[0] == 0.25);
    CHECK(loaded->identity.label.empty());
}

TEST_CASE("Reverb's factory rooms are built from the room table, not from a copy of it") {
    Scratch scratch("rooms");
    const auto& m = reverbParams();
    PresetLibrary lib(Product::Reverb, m, PluginState{m}, reverbFactoryPresets(), scratch.dir);
    CHECK(lib.folders(true) == std::vector<std::string>{"rooms"});
    REQUIRE(lib.all().size() == 7);
    CHECK(lib.all()[4] == PresetRef{true, "rooms", "hall"});

    for (int room = 0; room < 6; ++room) {
        const auto loaded = lib.load(lib.all()[static_cast<std::size_t>(room + 1)]);
        REQUIRE(loaded.has_value());
        int touched = 0;
        RoomState roomState;
        applyPreset(
            m, static_cast<ReverbPreset>(room),
            [&](int at, float normalised) {
                ++touched;
                CHECK(loaded->params[static_cast<std::size_t>(at)] ==
                      doctest::Approx(fromNormalised(m, at, normalised)));
            },
            roomState);
        CHECK(touched >= 7);
        //  and the room's selection and shape came with it, as state
        CHECK(loaded->room == roomState);
        CHECK(loaded->room.preset == room);
        //  and everything the table does not list is as a fresh instance has it
        PluginState expected{m};
        applyPreset(
            m, static_cast<ReverbPreset>(room),
            [&](int at, float normalised) {
                expected.params[static_cast<std::size_t>(at)] = fromNormalised(m, at, normalised);
            },
            expected.room);
        CHECK(sameSound(Product::Reverb, m, *loaded, expected));
    }
    CHECK_FALSE(sameSound(Product::Reverb, m, *lib.load(lib.all()[1]), *lib.load(lib.all()[6])));
}

namespace {
std::vector<std::string> shown(const std::vector<PresetRow>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) {
        if (r.kind == PresetRow::Kind::Group)
            out.push_back("[" + r.heading + (r.folded ? " folded]" : "]"));
        else if (r.kind == PresetRow::Kind::Folder)
            out.push_back("<" + r.heading + " " + std::to_string(r.count) + (r.folded ? " folded>" : ">"));
        else
            out.push_back((r.inFolder ? "  " : "") + r.ref.name + (r.tag.empty() ? "" : " #" + r.tag));
    }
    return out;
}
}  // namespace

TEST_CASE(
    "the browser's rows: a tree unsearched, folded where it is folded; flat hits with their folder when searched") {
    const std::vector<PresetRef> all{kDefaultPreset,       {true, "rooms", "hall"},    {true, "rooms", "room"},
                                     {false, "", "verse"}, {false, "songs", "bridge"}, {false, "songs", "outro"}};
    const auto none = [](std::string_view) { return false; };

    CHECK(shown(presetRows(all, "", none)) == std::vector<std::string>{"[factory]", "default", "<factory/rooms 2>",
                                                                       "  hall", "  room", "[user]", "verse",
                                                                       "<user/songs 2>", "  bridge", "  outro"});

    const auto songsFolded = [](std::string_view key) { return key == "user/songs"; };
    CHECK(shown(presetRows(all, "", songsFolded)) ==
          std::vector<std::string>{"[factory]", "default", "<factory/rooms 2>", "  hall", "  room", "[user]", "verse",
                                   "<user/songs 2 folded>"});

    const auto factoryFolded = [](std::string_view key) { return key == "factory"; };
    CHECK(shown(presetRows(all, "", factoryFolded)) ==
          std::vector<std::string>{"[factory folded]", "[user]", "verse", "<user/songs 2>", "  bridge", "  outro"});

    //  a search finds what is folded away, lists it flat with its folder, and drops a group with no hit
    CHECK(shown(presetRows(all, "songs", songsFolded)) ==
          std::vector<std::string>{"[user]", "bridge #songs", "outro #songs"});
    CHECK(shown(presetRows(all, "r", factoryFolded)) == std::vector<std::string>{"[factory]", "hall #rooms",
                                                                                 "room #rooms", "[user]", "verse",
                                                                                 "bridge #songs", "outro #songs"});
    CHECK(presetRows(all, "zzz", none).empty());

    //  with no user preset at all the heading is still there: it says where a save goes
    const std::vector<PresetRef> fresh{kDefaultPreset};
    CHECK(shown(presetRows(fresh, "", none)) == std::vector<std::string>{"[factory]", "default", "[user]"});
}

TEST_CASE("the window's size is the session's: saved with it, sparse, clamped, and no preset's") {
    //  Catches: the size not written, written at the default, read unclamped, carried
    //  by a preset file, taken from a preset's document, or overwritten by a preset load.
    const auto& m = reverbParams();
    PluginState s{m};
    CHECK(saveState(Product::Reverb, m, s).find("\"window\"") == std::string::npos);
    s.windowScale = 1.5f;
    PluginState back{m};
    REQUIRE(loadState(Product::Reverb, m, saveState(Product::Reverb, m, s), back).ok);
    CHECK(back.windowScale == 1.5f);

    s.windowScale = 9.0f;  // a hand-edited or foreign document
    REQUIRE(loadState(Product::Reverb, m, saveState(Product::Reverb, m, s), back).ok);
    CHECK(back.windowScale == kWindowScaleMax);

    s.windowScale = 1.5f;
    CHECK(presetText(Product::Reverb, m, s).find("\"window\"") == std::string::npos);
    PluginState fromFile{m};
    REQUIRE(loadPresetText(Product::Reverb, m, saveState(Product::Reverb, m, s), fromFile).ok);
    CHECK(fromFile.windowScale == 1.0f);

    PluginState mine{m};
    mine.windowScale = 0.75f;
    adoptPreset(Product::Reverb, m, s, {false, "", "big"}, mine);
    CHECK(mine.windowScale == 0.75f);
}
