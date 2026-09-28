// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

#include "bambi/patch/parameters.hpp"
#include "doctest.h"

/*  Checks the frozen target lists. Which parameters answer modulation is decided by
 *  key strings, where a misspelling is silently "not modulatable" and a loose match silently makes
 *  something else a target. Each plugin names its targets and their kinds in full, and this fails on
 *  a key dropped, added, misspelled, or moved between kinds.
 */
namespace bambi::test {

struct Target {
    std::string key;
    DestKind kind;
};

/// A region slot's targets, the same in every plugin (`sharedDestKind`).
inline std::vector<Target> regionTargets(const std::string& slot) {
    return {{slot + ".yaw_rate", DestKind::Rate},
            {slot + ".pitch_rate", DestKind::Rate},
            {slot + ".roll_rate", DestKind::Rate},
            {slot + ".yaw", DestKind::DirectAngle},
            {slot + ".pitch", DestKind::DirectAngle},
            {slot + ".roll", DestKind::DirectAngle},
            {slot + ".evolve", DestKind::DirectAngle},  // clouds' one matrix target
            {slot + ".size", DestKind::DirectScalar},
            {slot + ".softness", DestKind::DirectScalar},
            //  the shape's every continuous setting
            {slot + ".band_elevation", DestKind::DirectScalar},
            {slot + ".thickness", DestKind::DirectScalar},
            {slot + ".fill", DestKind::DirectScalar},
            {slot + ".dot_size", DestKind::DirectScalar},
            {slot + ".coverage", DestKind::DirectScalar},
            {slot + ".contrast", DestKind::DirectScalar},
            {slot + ".detail", DestKind::DirectScalar}};
}

/// Every key named has its kind, and nothing else in the manifest is modulatable.
inline void checkTargets(const ParamManifest& m, const std::vector<Target>& expected) {
    for (const Target& t : expected) {
        const int at = m.byKey(t.key);
        INFO(t.key);
        REQUIRE(at != kNoParam);  // the list names a key the manifest does not have
        CHECK(m.destKindOf(at) == t.kind);
    }
    int modulatable = 0;
    for (int i = 0; i < m.size(); ++i)
        if (m.destKindOf(i) != DestKind::NotModulatable) {
            ++modulatable;
            bool named = false;
            for (const Target& t : expected) named = named || t.key == m[i].key;
            INFO(std::string(m[i].key));
            CHECK(named);  // modulatable, and in no list
        }
    CHECK(modulatable == static_cast<int>(expected.size()));
}

}  // namespace bambi::test
