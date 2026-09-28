// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/presets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace bambi {
namespace fs = std::filesystem;

namespace {
constexpr std::string_view kExtension = ".json";
constexpr std::string_view kFoldedFile = ".folded";
constexpr std::size_t kLongestName = 64;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// One path segment as a file name can hold it: trimmed, no separators or reserved characters, no leading dot.
std::string segment(std::string_view typed) {
    std::string out;
    for (const char c : typed) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) continue;
        out.push_back(c);
    }
    const auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!out.empty() && (blank(static_cast<unsigned char>(out.front())) || out.front() == '.'))
        out.erase(out.begin());
    while (!out.empty() && blank(static_cast<unsigned char>(out.back()))) out.pop_back();
    if (out.size() > kLongestName) out.resize(kLongestName);
    return out;
}

bool before(const std::string& a, const std::string& b) {
    const auto la = lower(a), lb = lower(b);
    return la != lb ? la < lb : a < b;
}

PresetRef factoryRef(std::string_view path) {
    const auto slash = path.find('/');
    if (slash == std::string_view::npos) return {true, "", std::string(path)};
    return {true, std::string(path.substr(0, slash)), std::string(path.substr(slash + 1))};
}

/// A group in list order: the loose ones, then folder by folder, folders as `folderOrder` has them.
void appendGroup(std::vector<PresetRef>& out, const std::vector<PresetRef>& group,
                 const std::vector<std::string>& folderOrder) {
    for (const auto& r : group)
        if (r.folder.empty()) out.push_back(r);
    for (const auto& f : folderOrder)
        for (const auto& r : group)
            if (r.folder == f) out.push_back(r);
}

}  // namespace

std::optional<PresetRef> parsePresetPath(std::string_view typed) {
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= typed.size()) {
        const auto slash = std::min(typed.find('/', at), typed.size());
        if (auto s = segment(typed.substr(at, slash - at)); !s.empty()) parts.push_back(std::move(s));
        at = slash + 1;
    }
    if (parts.empty()) return std::nullopt;
    if (parts.size() == 1) return PresetRef{false, "", parts[0]};
    std::string name = parts[1];
    for (std::size_t i = 2; i < parts.size(); ++i) name += " " + parts[i];
    if (name.size() > kLongestName) name.resize(kLongestName);
    return PresetRef{false, parts[0], name};
}

std::string presetPath(const PresetRef& ref) { return ref.folder.empty() ? ref.name : ref.folder + "/" + ref.name; }

std::span<const std::string_view> presetKeptKeys(Product p) {
    static constexpr std::array<std::string_view, 1> kEncoder{"input.mode"};
    return p == Product::Encoder ? std::span<const std::string_view>(kEncoder) : std::span<const std::string_view>{};
}

std::string presetText(Product p, const ParamManifest& m, const PluginState& s) {
    Json j = toJson(p, m, s);
    j.erase("identity");
    j.erase("preset");
    j.erase("window");  // the session's, not the sound's
    if (const Json* params = j.find("parameters"); params != nullptr && !presetKeptKeys(p).empty()) {
        Json kept = *params;
        for (const auto key : presetKeptKeys(p)) kept.erase(key);
        j.set("parameters", kept);
    }
    return j.dump(2);
}

LoadResult loadPresetText(Product p, const ParamManifest& m, std::string_view text, PluginState& out) {
    PluginState s{m};
    auto r = loadState(p, m, text, s);
    if (!r.ok) return r;
    s.identity = {};
    s.preset = {};
    s.windowScale = PluginState{}.windowScale;
    out = std::move(s);
    return r;
}

void adoptPreset(Product p, const ParamManifest& m, const PluginState& preset, const PresetRef& ref,
                 PluginState& into) {
    PluginState next = preset;
    next.identity = into.identity;
    next.windowScale = into.windowScale;
    next.preset = ref;
    for (const auto key : presetKeptKeys(p))
        if (const int at = m.byKey(key); at != kNoParam)
            next.params[static_cast<std::size_t>(at)] = into.params[static_cast<std::size_t>(at)];
    into = std::move(next);
}

bool sameSound(Product p, const ParamManifest& m, const PluginState& a, const PluginState& b) {
    if (a.trajectory != b.trajectory || a.matrix != b.matrix || a.sources != b.sources ||
        a.envTriggers != b.envTriggers || a.regions != b.regions || a.renderQuality != b.renderQuality)
        return false;

    const auto kept = presetKeptKeys(p);
    for (int i = 0; i < m.size(); ++i) {
        if (std::find(kept.begin(), kept.end(), m[i].key) != kept.end()) continue;
        const float tolerance = 1.0e-4f * (m[i].max - m[i].min);
        if (std::abs(a.params[static_cast<std::size_t>(i)] - b.params[static_cast<std::size_t>(i)]) > tolerance)
            return false;
    }
    return true;
}

std::vector<PresetRef> searchPresets(std::span<const PresetRef> all, std::string_view query) {
    while (!query.empty() && query.front() == ' ') query.remove_prefix(1);
    while (!query.empty() && query.back() == ' ') query.remove_suffix(1);
    const auto needle = lower(query);
    std::vector<PresetRef> out;
    for (const auto& r : all)
        if (needle.empty() || lower(presetPath(r)).find(needle) != std::string::npos) out.push_back(r);
    return out;
}

PresetRef stepPreset(std::span<const PresetRef> all, const PresetRef& current, int delta) {
    if (all.empty()) return current;
    const auto found = std::find(all.begin(), all.end(), current);
    const auto at = found == all.end() ? 0 : static_cast<long>(found - all.begin());
    const auto n = static_cast<long>(all.size());
    return all[static_cast<std::size_t>(((at + delta) % n + n) % n)];
}

std::string presetHeading(bool factory, std::string_view folder) {
    std::string key = factory ? "factory" : "user";
    if (!folder.empty()) key += "/" + std::string(folder);
    return key;
}

std::vector<PresetRow> presetRows(std::span<const PresetRef> all, std::string_view query,
                                  const std::function<bool(std::string_view)>& folded) {
    using Kind = PresetRow::Kind;
    while (!query.empty() && query.front() == ' ') query.remove_prefix(1);
    const bool searching = !query.empty();
    const auto hits = searchPresets(all, query);
    const auto isFolded = [&](const std::string& key) { return !searching && folded && folded(key); };

    std::vector<PresetRow> rows;
    for (const bool factory : {true, false}) {
        std::vector<PresetRef> group;
        for (const auto& r : hits)
            if (r.factory == factory) group.push_back(r);
        if (searching && group.empty()) continue;

        const auto groupKey = presetHeading(factory);
        rows.push_back(
            {Kind::Group, {factory, "", ""}, groupKey, isFolded(groupKey), static_cast<int>(group.size()), false, ""});
        if (rows.back().folded) continue;

        std::string open;  // the folder whose heading was last written
        bool openFolded = false;
        for (const auto& r : group) {
            if (searching) {
                rows.push_back({Kind::Preset, r, "", false, 0, false, r.folder});
                continue;
            }
            if (!r.folder.empty() && r.folder != open) {
                open = r.folder;
                const auto key = presetHeading(factory, open);
                openFolded = isFolded(key);
                const auto count =
                    std::count_if(group.begin(), group.end(), [&](const PresetRef& o) { return o.folder == open; });
                rows.push_back(
                    {Kind::Folder, {factory, open, ""}, key, openFolded, static_cast<int>(count), false, ""});
            }
            if (r.folder.empty() || !openFolded) rows.push_back({Kind::Preset, r, "", false, 0, !r.folder.empty(), ""});
        }
    }
    return rows;
}

// ---- the library -----------------------------------------------------------------------------

PresetLibrary::PresetLibrary(Product p, const ParamManifest& m, PluginState fresh,
                             std::span<const FactoryPreset> factory, fs::path userRoot, Dispose dispose)
    : product_(p),
      m_(m),
      fresh_(std::move(fresh)),
      factory_(factory.begin(), factory.end()),
      root_(std::move(userRoot)),
      dispose_(std::move(dispose)) {
    fresh_.identity = {};
    fresh_.preset = {};
    readFolded();
    scan();
}

void PresetLibrary::scan() {
    referenceOf_ = {};  // what a name refers to may have changed on the disk
    reference_.reset();
    std::vector<PresetRef> factory{kDefaultPreset};
    std::vector<std::string> factoryFolders;
    for (const auto& f : factory_) {
        auto ref = factoryRef(f.path);
        if (!ref.folder.empty() &&
            std::find(factoryFolders.begin(), factoryFolders.end(), ref.folder) == factoryFolders.end())
            factoryFolders.push_back(ref.folder);
        factory.push_back(std::move(ref));
    }

    std::vector<PresetRef> user;
    std::vector<std::string> userFolders;
    std::error_code ec;
    const auto take = [&](const fs::path& dir, const std::string& folder) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto& path = it->path();
            if (it->is_regular_file(ec) && path.extension() == kExtension && path.stem().string().front() != '.')
                user.push_back({false, folder, path.stem().string()});
        }
    };
    take(root_, "");
    for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || it->path().filename().string().front() == '.') continue;
        const auto folder = it->path().filename().string();
        const auto count = user.size();
        take(it->path(), folder);
        if (user.size() > count) userFolders.push_back(folder);
    }
    std::sort(user.begin(), user.end(), [](const PresetRef& a, const PresetRef& b) { return before(a.name, b.name); });
    std::sort(userFolders.begin(), userFolders.end(), before);

    all_.clear();
    appendGroup(all_, factory, factoryFolders);
    appendGroup(all_, user, userFolders);
}

std::vector<std::string> PresetLibrary::folders(bool factory) const {
    std::vector<std::string> out;
    for (const auto& r : all_)
        if (r.factory == factory && !r.folder.empty() && (out.empty() || out.back() != r.folder))
            out.push_back(r.folder);
    return out;
}

bool PresetLibrary::has(const PresetRef& ref) const { return std::find(all_.begin(), all_.end(), ref) != all_.end(); }

fs::path PresetLibrary::fileOf(const PresetRef& ref) const {
    if (ref.factory || ref.none()) return {};
    auto dir = ref.folder.empty() ? root_ : root_ / ref.folder;
    return dir / (ref.name + std::string(kExtension));
}

std::optional<PluginState> PresetLibrary::load(const PresetRef& ref) const {
    if (ref.factory) {
        if (ref == kDefaultPreset) return fresh_;
        for (const auto& f : factory_)
            if (factoryRef(f.path) == ref) {
                PluginState s = fresh_;
                f.build(m_, s);
                return s;
            }
        return std::nullopt;
    }
    std::ifstream in(fileOf(ref), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    PluginState s{m_};
    if (!loadPresetText(product_, m_, text.str(), s).ok) return std::nullopt;
    return s;
}

bool PresetLibrary::modified(const PluginState& live) {
    if (live.preset.none()) return false;
    if (!(live.preset == referenceOf_)) {
        reference_ = load(live.preset);
        referenceOf_ = live.preset;
    }
    return !reference_.has_value() || !sameSound(product_, m_, live, *reference_);
}

bool PresetLibrary::save(const PresetRef& ref, const PluginState& s) {
    const auto file = fileOf(ref);
    if (file.empty()) return false;  // a factory preset, or no name
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return false;
    //  to a file beside it first: a write that fails half way leaves the preset that was there
    auto partial = file;
    partial += ".part";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << presetText(product_, m_, s);
        if (!out) return false;
    }
    fs::rename(partial, file, ec);
    if (ec) {
        fs::remove(partial, ec);
        return false;
    }
    scan();
    return true;
}

bool PresetLibrary::rename(const PresetRef& from, const PresetRef& to) {
    if (from.factory || to.factory || to.none() || !has(from)) return false;
    if (from == to) return true;
    std::error_code ec;
    //  a name that differs only in case is the same file where the disk ignores case: let it through
    if (fs::exists(fileOf(to), ec) && lower(presetPath(from)) != lower(presetPath(to))) return false;
    fs::create_directories(fileOf(to).parent_path(), ec);
    fs::rename(fileOf(from), fileOf(to), ec);
    if (ec) return false;
    dropEmptyFolders();
    scan();
    return true;
}

bool PresetLibrary::remove(const PresetRef& ref) {
    if (ref.factory || !has(ref)) return false;
    std::error_code ec;
    const bool gone = dispose_ ? dispose_(fileOf(ref)) : fs::remove(fileOf(ref), ec);
    if (!gone) return false;
    dropEmptyFolders();
    scan();
    return true;
}

void PresetLibrary::dropEmptyFolders() {
    //  Empty of anything a person put there: the Finder's own dotfiles do not keep a folder.
    std::error_code ec;
    std::vector<fs::path> empty;
    for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        bool kept = false;
        for (fs::directory_iterator in(it->path(), ec), last; !ec && in != last && !kept; in.increment(ec))
            kept = in->path().filename().string().front() != '.';
        if (!kept) empty.push_back(it->path());
    }
    for (const auto& dir : empty) fs::remove_all(dir, ec);
}

// ---- which headings are folded ---------------------------------------------------------------

bool PresetLibrary::folded(std::string_view heading) const {
    return std::find(folded_.begin(), folded_.end(), heading) != folded_.end();
}

void PresetLibrary::setFolded(std::string_view heading, bool isFolded) {
    if (folded(heading) == isFolded) return;
    if (isFolded)
        folded_.emplace_back(heading);
    else
        std::erase(folded_, heading);
    writeFolded();
}

void PresetLibrary::readFolded() {
    folded_.clear();
    std::ifstream in(root_ / kFoldedFile);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) folded_.push_back(line);
}

void PresetLibrary::writeFolded() const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    std::ofstream out(root_ / kFoldedFile, std::ios::trunc);
    for (const auto& heading : folded_) out << heading << '\n';
}

}  // namespace bambi
