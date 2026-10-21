// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/live/library.hpp"

#include <utility>
#include <vector>

namespace bambi::live {

namespace {

/// The dotted numbers after "Live " ("12.4.6" -> 12, 4, 6), up to the first character that is neither, then 1
/// for a release or 0 when anything follows (a beta, "12.2b5"), so a release beats its own beta; empty when the
/// name is not a Live version's. A number is read to six digits at most.
std::vector<int> liveVersion(std::string_view name) {
    constexpr std::string_view prefix = "Live ";
    if (!name.starts_with(prefix)) return {};
    const auto digit = [&](std::size_t i) { return i < name.size() && name[i] >= '0' && name[i] <= '9'; };
    std::vector<int> parts;
    std::size_t i = prefix.size();
    while (digit(i)) {
        int n = 0;
        for (int d = 0; digit(i); ++i)
            if (d++ < 6) n = n * 10 + (name[i] - '0');
        parts.push_back(n);
        if (i < name.size() && name[i] == '.' && digit(i + 1))
            ++i;
        else
            break;
    }
    if (!parts.empty()) parts.push_back(i == name.size() ? 1 : 0);
    return parts;
}

/// An XML attribute's value with the five named entities replaced by their characters.
std::string unescape(std::string_view s) {
    constexpr std::pair<std::string_view, char> entities[] = {
        {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&')
            for (auto [name, c] : entities)
                if (s.substr(i).starts_with(name)) {
                    out += c;
                    i += name.size();
                    replaced = true;
                    break;
                }
        if (!replaced) out += s[i++];
    }
    return out;
}

}  // namespace

std::string newestLiveFolder(std::span<const std::string> folders) {
    std::string best;
    std::vector<int> bestVersion;
    for (const auto& f : folders) {
        auto v = liveVersion(f);
        if (!v.empty() && (bestVersion.empty() || v > bestVersion)) {
            best = f;
            bestVersion = std::move(v);
        }
    }
    return best;
}

std::string userLibraryFromConfig(std::string_view cfg) {
    const auto open = cfg.find("<UserLibrary>");
    if (open == std::string_view::npos) return {};
    const auto close = cfg.find("</UserLibrary>", open);
    const auto section = cfg.substr(open, close == std::string_view::npos ? std::string_view::npos : close - open);
    const auto attribute = [&](std::string_view element) -> std::string {
        const auto key = "<" + std::string(element) + " Value=\"";
        const auto at = section.find(key);
        if (at == std::string_view::npos) return {};
        const auto start = at + key.size();
        const auto end = section.find('"', start);
        return end == std::string_view::npos ? std::string() : unescape(section.substr(start, end - start));
    };
    auto path = attribute("ProjectPath");
    if (path.empty()) return {};
    auto name = attribute("ProjectName");
    if (path.back() != '/') path += '/';
    return path + (name.empty() ? "User Library" : name);
}

}  // namespace bambi::live
