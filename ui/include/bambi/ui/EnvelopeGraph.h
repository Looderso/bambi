// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <juce_gui_basics/juce_gui_basics.h>

#include "bambi/mod/sources.hpp"
#include "bambi/patch/parameters.hpp"

/*  An envelope's shape, dragged: the picture every plugin's envelope page draws -- a line at each
 *  stage's edge, the hold shaded, the bowed curve, a point at each corner and a diamond at each bowed
 *  stage's midpoint. Points set times and the sustain level; diamonds set only the curve.
 *
 *  A class, not a free function, because a gesture has to outlive the paint that started it: the
 *  plot's scale is frozen while something is held. It knows no plugin's parameter enum: the ids it
 *  edits are handed to it, like everything in `ui/`.
 */
namespace bambi::ui {

class ParameterPage;

class EnvelopeGraph {
public:
    /// The eight parameters the picture shows, in the order the stages run.
    struct Ids {
        ParamId attack{kNoParamId}, attackCurve{kNoParamId};
        ParamId decay{kNoParamId}, decayCurve{kNoParamId};
        ParamId sustain{kNoParamId};
        ParamId release{kNoParamId}, releaseCurve{kNoParamId};
    };

    /// Draws the plot at `y`, registers what it drew, and returns the y to carry on at.
    float paint(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y, const Ids& ids,
                double polarity);

    /// What is being dragged, for a check and for drawing the held one brighter.
    EnvelopeGrab held() const { return held_; }

    /// Where the last paint put them, so a check drags a handle where it really is. Empty until one is painted.
    juce::Rectangle<float> plotArea() const { return plot_; }
    juce::Point<float> handleAt(EnvelopeGrab grab) const { return handles_[index(grab)]; }

private:
    static std::size_t index(EnvelopeGrab grab) { return static_cast<std::size_t>(grab); }

    juce::Rectangle<float> plot_;
    std::array<juce::Point<float>, 8> handles_{};
    EnvelopeGrab held_{EnvelopeGrab::None};
    double span_{0.0};   ///< frozen for the duration of a gesture
    float lastY_{0.0f};  ///< a bow and the sustain move by a delta, not to a position
};

}  // namespace bambi::ui
