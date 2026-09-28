// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "Controls.h"
#include "bambi/echo/control.hpp"

namespace bambi::ui {

/*  Where a tap's passes land, as numbers.
 *
 *  Separated from the drawing so that what the strip shows can be checked without painting
 *  anything: a picture whose only assertion is "it did not crash" is a picture nobody has checked.
 *
 *  The passes come from the same `EchoResolver` the engine's control step uses -- not from a second
 *  reading of the parameters -- so the strip cannot say a period the plugin is not playing.
 */
struct Pass {
    double seconds{0.0};  ///< where it lands, from the start of the span
    double gain{1.0};     ///< linear: the tap's level times its feedback, j-1 times
    int index{1};         ///< j: the first pass is 1
};

struct TapPasses {
    bool on{false};
    double periodSeconds{0.0}, offsetSeconds{0.0};
    double lowCutHz{20.0}, highCutHz{20000.0}, blurRad{0.0};
    std::vector<Pass> passes;
};

struct StripContent {
    std::array<TapPasses, bambi::kEchoTaps> taps;
    double spanSeconds{1.0};  ///< the shared axis: the longest enabled tap, rounded up to a beat
    double beatSeconds{0.5};
};

/*  Walk every enabled tap to its stop -- 48 dB down, 4 seconds, or 32 passes -- and take the
 *  longest of them, rounded up to a whole beat so the grid stays musical and an offset can be
 *  read against it. The axis moves under all four taps whenever any one of them changes: that is
 *  the price of one shared axis, and it is what makes interleaving legible.
 */
StripContent stripContent(const bambi::PluginState& patch, double bpm, double sampleRate, int order);

/// The plugin's own band, exactly: a one-pole at the high cut, then that minus a one-pole of it at the low.
double bandMagnitude(double hz, double lowCutHz, double highCutHz, double sampleRate);

}  // namespace bambi::ui
