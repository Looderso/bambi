// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace bambi {

double stepSeconds(double bpm) { return 60.0 / std::max(bpm, 1.0) / 4.0; }

TapTimes tapTimes(const TapTiming& t, double bpm, int order) {
    const double cap = maxTapSeconds(order);
    TapTimes out;
    if (!t.synced) {
        out.periodSeconds = std::clamp(t.ms / 1000.0, kMinTapSeconds, cap);
        out.offsetSeconds = std::clamp(t.offsetMs / 1000.0, 0.0, cap);
        return out;
    }
    const double step = stepSeconds(bpm);
    int steps = std::clamp(t.steps, 1, kMaxSteps);
    while (steps > 1 && steps * step > cap) steps /= 2;
    while (steps < kMaxSteps && steps * step < kMinTapSeconds) steps *= 2;
    const double period = steps * step;
    const bool fits = period <= cap && period >= kMinTapSeconds;
    out.periodSeconds = std::clamp(period, kMinTapSeconds, cap);
    out.stepsPlayed = fits ? steps : 0;
    out.offsetSeconds =
        std::clamp((std::clamp(t.offsetSteps, 0, kMaxSteps) + std::clamp(t.swing, 0.0, kMaxSwing)) * step, 0.0, cap);
    return out;
}

TapTiming leaveSync(TapTiming t, double bpm) {
    const double step = stepSeconds(bpm);
    t.ms = std::clamp(std::clamp(t.steps, 1, kMaxSteps) * step, kMinTapSeconds, kMaxStoredSeconds) * 1000.0;
    t.offsetMs = std::clamp((std::clamp(t.offsetSteps, 0, kMaxSteps) + std::clamp(t.swing, 0.0, kMaxSwing)) * step, 0.0,
                            kMaxStoredSeconds) *
                 1000.0;
    t.synced = false;
    return t;
}

TapTiming returnToSync(TapTiming t, double bpm) {
    const double step = stepSeconds(bpm);
    t.steps = std::clamp(static_cast<int>(std::lround(t.ms / 1000.0 / step)), 1, kMaxSteps);
    const double offset = std::max(0.0, t.offsetMs / 1000.0) / step;
    const double whole = std::floor(offset + 1e-9);
    t.offsetSteps = std::clamp(static_cast<int>(whole), 0, kMaxSteps);
    t.swing = std::clamp(offset - whole, 0.0, kMaxSwing);
    t.synced = true;
    return t;
}

double placeInLoop(const TapTimes& times) {
    if (times.periodSeconds <= 0.0) return 0.0;
    return std::fmod(times.offsetSeconds, times.periodSeconds) / times.periodSeconds;
}

std::string_view stepName(int steps) {
    static constexpr std::array<std::string_view, 17> names{"",     "1/16",  "1/8",   "1/8.",  "1/4",   "5/16",
                                                            "1/4.", "7/16",  "1/2",   "9/16",  "10/16", "11/16",
                                                            "1/2.", "13/16", "14/16", "15/16", "1 bar"};
    return steps >= 1 && steps <= kMaxSteps ? names[static_cast<std::size_t>(steps)] : std::string_view{};
}

}  // namespace bambi
