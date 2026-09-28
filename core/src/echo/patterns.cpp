// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/patterns.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "bambi/echo/control.hpp"

namespace bambi {
namespace {

struct Tap {
    int steps, offset;
    double az, el, spin, skew, feedbackDb;
};

//  A tap a pattern leaves empty is off, with a fresh instance's settings.
using Row = std::array<std::optional<Tap>, kEchoTaps>;

constexpr std::array<Row, 5> kPatterns{{
    /* even      */ {Tap{2, 0, 0.0, 90.0, 0.0, 0.0, -6.0}},
    /* ping-pong */ {Tap{3, 0, 0.0, 90.0, 180.0, 0.0, -6.0}},
    /* spiral    */ {Tap{2, 0, 0.0, 90.0, 45.0, 0.0, -6.0}},
    /* cascade   */ {Tap{2, 0, 0.0, 90.0, 45.0, 0.3, -6.0}, Tap{2, 1, 0.0, 90.0, -45.0, 0.3, -6.0}},
    /* single    */ {Tap{2, 0, 0.0, 90.0, 0.0, 0.0, -24.0}},
}};

constexpr Tap fresh(const EchoTapSettings& t) {
    return {t.timing.steps, t.timing.offsetSteps, t.axisAzimuthDeg, t.axisElevationDeg, t.spinDeg,
            t.skew,         t.feedbackDb};
}

}  // namespace

void applyPattern(const ParamManifest& m, EchoPattern p, const std::function<void(int, float)>& write) {
    const Row& row = kPatterns[static_cast<std::size_t>(std::clamp(static_cast<int>(p), 0, 4))];
    for (int k = 0; k < kEchoTaps; ++k) {
        const auto i = static_cast<std::size_t>(k);
        const bool on = row[i].has_value();
        const Tap t = row[i].value_or(fresh(kEchoTapDefaults[i]));
        const std::string prefix = "tap" + std::to_string(k + 1) + ".";
        const auto set = [&](const char* key, double value) {
            const int at = m.byKey(prefix + key);
            if (at == kNoParam || !write) return;
            write(at, toNormalised(m, at, static_cast<float>(value)));
        };
        set("on", on ? 1.0 : 0.0);
        set("synced", 1.0);
        set("steps", t.steps);
        set("offset_steps", t.offset);
        set("az", t.az);
        set("el", t.el);
        set("spin", t.spin);
        set("skew", t.skew);
        set("feedback", t.feedbackDb);
    }
}

}  // namespace bambi
