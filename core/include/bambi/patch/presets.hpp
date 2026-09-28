// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"

namespace bambi {

/// A preset is the whole sound of one plugin -- every parameter, matrix, sources, triggers,
/// regions, the encoder's trajectory -- the state document without who the instance is. One file
/// per preset, `<root>/<folder>/<name>.json`, one folder deep; factory presets have no file.

struct FactoryPreset {
    std::string_view path;  ///< "folder/name", or "name"
    void (*build)(const ParamManifest&, PluginState&);
};

/// What every plugin's list starts with: the patch a fresh instance has.
inline const PresetRef kDefaultPreset{true, "", "default"};

/// What a typed name means: "drums/tight room" is `tight room` in folder `drums`; a further slash
/// joins the name. Characters a file name can't hold, and leading dots, are dropped. Always a user preset.
std::optional<PresetRef> parsePresetPath(std::string_view typed);
/// "folder/name", or "name": what `parsePresetPath` reads back.
std::string presetPath(const PresetRef& ref);

/// The keys a preset leaves as they are: the encoder's input mode says what is plugged in, not
/// how it sounds, and loading a preset must not silently rewire the track.
std::span<const std::string_view> presetKeptKeys(Product p);

std::string presetText(Product p, const ParamManifest& m, const PluginState& s);
LoadResult loadPresetText(Product p, const ParamManifest& m, std::string_view text, PluginState& out);

/// Make `into` the preset: everything but who the instance is and the kept keys, plus `ref` as its new name.
void adoptPreset(Product p, const ParamManifest& m, const PluginState& preset, const PresetRef& ref, PluginState& into);

/// Whether two patches sound the same: everything a preset carries, kept keys aside. A parameter is
/// compared within a ten-thousandth of its range, since a host round-trip isn't bit-exact.
bool sameSound(Product p, const ParamManifest& m, const PluginState& a, const PluginState& b);

/// The presets whose path contains `query`, case-insensitively, in the order given. An empty query is all of them.
std::vector<PresetRef> searchPresets(std::span<const PresetRef> all, std::string_view query);
/// The preset `delta` places on from `current`, wrapping. One that is not listed counts as the first.
PresetRef stepPreset(std::span<const PresetRef> all, const PresetRef& current, int delta);

/// What the browser lists, row by row. Unsearched it's the tree: group heading, folder heading,
/// presets under them, nothing under a folded heading. A search lists hits flat under their
/// group, tagged with their folder, ignoring anything folded.
struct PresetRow {
    enum class Kind { Group, Folder, Preset };
    Kind kind{Kind::Preset};
    PresetRef ref;        ///< a preset's own; a heading's says which group and folder
    std::string heading;  ///< folding key: "user", "user/songs"; empty for a preset
    bool folded{false};
    int count{0};          ///< presets a folder holds
    bool inFolder{false};  ///< drawn under its folder's heading
    std::string tag;       ///< a search hit's folder

    friend bool operator==(const PresetRow&, const PresetRow&) = default;
};
std::vector<PresetRow> presetRows(std::span<const PresetRef> all, std::string_view query,
                                  const std::function<bool(std::string_view heading)>& folded);
/// The heading key of a group, or of a folder in it.
std::string presetHeading(bool factory, std::string_view folder = {});

class PresetLibrary {
public:
    using Dispose = std::function<bool(const std::filesystem::path&)>;  ///< e.g. move to the Trash; unset deletes

    /// `fresh` is the patch a new instance has and what "default" loads; `userRoot` need not exist yet.
    PresetLibrary(Product p, const ParamManifest& m, PluginState fresh, std::span<const FactoryPreset> factory,
                  std::filesystem::path userRoot, Dispose dispose = {});

    /// Read the user directory again. Every call that changes it does this itself.
    void scan();

    /// List order: factory then user, loose entries then folder by folder; factory keeps write
    /// order, user is alphabetical.
    const std::vector<PresetRef>& all() const { return all_; }
    /// A group's folders, in list order.
    std::vector<std::string> folders(bool factory) const;
    bool has(const PresetRef& ref) const;

    std::optional<PluginState> load(const PresetRef& ref) const;
    /// Whether `live` has moved from the preset it names. Its file is read once and cached until
    /// the name changes; a missing file counts as moved, no preset never does.
    bool modified(const PluginState& live);
    /// Write a user preset, over one of the same name. False for a factory preset or a failed write.
    bool save(const PresetRef& ref, const PluginState& s);
    /// Rename or move a user preset. Refused when `to` exists. A folder left empty goes.
    bool rename(const PresetRef& from, const PresetRef& to);
    /// Remove a user preset's file, the way `dispose` says. A folder left empty goes.
    bool remove(const PresetRef& ref);

    std::filesystem::path fileOf(const PresetRef& ref) const;
    const std::filesystem::path& root() const { return root_; }

    /// Which headings are folded: "factory", "user", "factory/<folder>", "user/<folder>". Kept in
    /// the user directory: belongs to the user, not a session.
    bool folded(std::string_view heading) const;
    void setFolded(std::string_view heading, bool folded);

private:
    void dropEmptyFolders();
    void readFolded();
    void writeFolded() const;

    Product product_;
    const ParamManifest& m_;
    PluginState fresh_;
    std::vector<FactoryPreset> factory_;
    std::filesystem::path root_;
    Dispose dispose_;
    std::vector<PresetRef> all_;
    std::vector<std::string> folded_;
    PresetRef referenceOf_;                 ///< which preset `reference_` is
    std::optional<PluginState> reference_;  ///< that preset as its file has it, for `modified`
};

}  // namespace bambi
