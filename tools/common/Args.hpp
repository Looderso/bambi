// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <initializer_list>
#include <string>

/*  A tool's arguments read whole: a number that does not parse to its last character is refused, never read as the
 *  0 that atoi and atof would make of it. */
namespace bambi::tools {

inline bool whole(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || end != text.c_str() + text.size()) return false;
    out = static_cast<int>(v);
    return true;
}

inline bool whole(const std::string& text, long& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || end != text.c_str() + text.size()) return false;
    out = v;
    return true;
}

inline bool decimal(const std::string& text, double& out) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size()) return false;
    out = v;
    return true;
}

/// "A,B" or "A,B,C": every part a decimal, and exactly as many as asked for.
inline bool decimals(const std::string& text, std::initializer_list<double*> out) {
    std::size_t from = 0, k = 0;
    for (double* o : out) {
        const std::size_t comma = text.find(',', from);
        const bool last = ++k == out.size();
        if (last != (comma == std::string::npos)) return false;
        if (!decimal(text.substr(from, last ? std::string::npos : comma - from), *o)) return false;
        from = comma + 1;
    }
    return true;
}

}  // namespace bambi::tools
