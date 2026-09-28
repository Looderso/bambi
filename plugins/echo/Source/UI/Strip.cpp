// SPDX-License-Identifier: GPL-3.0-or-later
#include "Strip.h"

#include <cmath>

namespace bambi::ui {

namespace {
//  where the walk stops: below this floor, past this span, or after this many passes.
constexpr int kMaxPasses = 32;
constexpr double kMaxSeconds = 4.0;
constexpr double kFloorDb = -48.0;
}  // namespace

double bandMagnitude(double hz, double lowCutHz, double highCutHz, double sampleRate) {
    const double w = 2.0 * bambi::kPi * hz / sampleRate;
    const double cw = std::cos(w), sw = std::sin(w);
    const auto onePole = [sampleRate](double fc) {
        return 1.0 - std::exp(-2.0 * bambi::kPi * std::min(fc, 0.45 * sampleRate) / sampleRate);
    };
    const double a = onePole(highCutHz), b = onePole(lowCutHz);
    const double magHi = a / std::hypot(1.0 - (1.0 - a) * cw, (1.0 - a) * sw);
    const double magLo = (1.0 - b) * std::hypot(1.0 - cw, sw) / std::hypot(1.0 - (1.0 - b) * cw, (1.0 - b) * sw);
    return magHi * magLo;
}

StripContent stripContent(const bambi::PluginState& patch, double bpm, double sampleRate, int order) {
    StripContent out;
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    out.beatSeconds = 60.0 / std::max(20.0, bpm);

    /*  Through the plugin's own control step: a fresh resolver, because nothing here needs the
        period held against a wobbling tempo, and the same `echoSettingsFrom` the engine reads. */
    bambi::EchoResolver resolver;
    const auto settings = bambi::echoSettingsFrom(bambi::echoParams(), patch.params, patch.regions[0].shape);
    const bambi::EchoFrame frame = resolver.resolve(settings, bpm, rate, std::max(1, order));

    double longest = 0.0;
    for (int i = 0; i < bambi::kEchoTaps; ++i) {
        const auto& t = frame.taps[static_cast<std::size_t>(i)];
        auto& into = out.taps[static_cast<std::size_t>(i)];
        into.on = t.on;
        into.periodSeconds = static_cast<double>(t.periodSamples) / rate;
        into.offsetSeconds = static_cast<double>(t.offsetSamples) / rate;
        into.lowCutHz = t.pass.lowCutHz;
        into.highCutHz = t.pass.highCutHz;
        into.blurRad = t.pass.blurRad;
        if (!t.on || into.periodSeconds <= 0.0) continue;

        double gain = t.level;
        for (int j = 1; j <= kMaxPasses; ++j) {
            const double at = into.offsetSeconds + static_cast<double>(j) * into.periodSeconds;
            const double db = 20.0 * std::log10(std::max(1e-9, gain));
            if (at > kMaxSeconds || db < kFloorDb) break;
            into.passes.push_back({at, gain, j});
            longest = std::max(longest, at);
            gain *= t.feedback;
        }
    }

    //  rounded up to a whole beat, so the bar and beat marks stay readable
    const double beats = std::ceil(std::max(longest, out.beatSeconds) / out.beatSeconds);
    out.spanSeconds = beats * out.beatSeconds;
    return out;
}

}  // namespace bambi::ui
