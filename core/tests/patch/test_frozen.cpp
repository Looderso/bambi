// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include "bambi/decode/params.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/reverb/params.hpp"
#include "doctest.h"

using namespace bambi;

/*  What a host's automation means, frozen with the key it is saved under.
 *
 *  A host stores an automation point as a position from 0 to 1, so a parameter's type, range, skew and
 *  number of choices decide what every saved point plays. `frozen-parameters.txt` holds one line of
 *  them per parameter of every plugin. A line may be appended, never changed or removed; a name, a unit
 *  or a default may change freely, since no saved point depends on them.
 *
 *  BAMBI_FREEZE_WRITE=1 rewrites the file from the manifests -- for appending. Review its diff: a changed
 *  line is a break.
 */
namespace {

std::string frozenFile() { return std::string(BAMBI_CORE_DIR) + "/tests/patch/frozen-parameters.txt"; }

/// "<plugin> <key> <type> <min> <max> <skew centre> <choices>", per parameter.
std::map<std::string, std::string> linesOf() {
    std::map<std::string, std::string> out;
    const std::pair<const char*, const ParamManifest*> plugins[] = {{"encoder", &encodeParams()},
                                                                    {"echo", &echoParams()},
                                                                    {"reverb", &reverbParams()},
                                                                    {"decoder", &decodeParams()}};
    for (const auto& [plugin, m] : plugins)
        for (int at = 0; at < m->size(); ++at) {
            const ParamDesc& d = (*m)[at];
            int choices = 0;
            if (d.type == ParamType::Choice) {
                choices = 1;
                for (const char c : d.choices) choices += c == ',' ? 1 : 0;
            }
            char line[256];
            std::snprintf(line, sizeof line, "%s %.*s %s %.9g %.9g %.9g %d", plugin, static_cast<int>(d.key.size()),
                          d.key.data(),
                          d.type == ParamType::Float    ? "float"
                          : d.type == ParamType::Choice ? "choice"
                                                        : "bool",
                          static_cast<double>(d.min), static_cast<double>(d.max),
                          static_cast<double>(m->skewCentreOf(at)), choices);
            out[std::string(plugin) + " " + std::string(d.key)] = line;
        }
    return out;
}

}  // namespace

TEST_CASE("every parameter's key, type, range, skew and choices are frozen") {
    //  Catches: a range, skew or choice list changed under a released key, which moves every automation
    //  point saved against it; a key removed or renamed; and a new key nobody froze.
    const auto now = linesOf();
    if (const char* write = std::getenv("BAMBI_FREEZE_WRITE"); write != nullptr && std::string(write) == "1") {
        std::ofstream f(frozenFile());
        for (const auto& [key, line] : now) f << line << "\n";
    }
    std::ifstream f(frozenFile());
    REQUIRE_MESSAGE(f.good(), "no " << frozenFile());
    std::map<std::string, std::string> frozen;
    for (std::string line; std::getline(f, line);) {
        if (line.empty()) continue;
        std::istringstream words(line);
        std::string plugin, key;
        words >> plugin >> key;
        std::string id = plugin;
        id += ' ';
        id += key;
        frozen[id] = line;
    }
    for (const auto& [key, line] : frozen) {
        const auto at = now.find(key);
        CHECK_MESSAGE(at != now.end(), "removed: " << line);
        if (at != now.end()) CHECK_MESSAGE(at->second == line, "was: " << line << "\n  now: " << at->second);
    }
    for (const auto& [key, line] : now) CHECK_MESSAGE(frozen.count(key) == 1, "not frozen yet: " << line);
}
