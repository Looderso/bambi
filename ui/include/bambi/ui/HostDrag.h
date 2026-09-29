// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/PatchModel.h"

/// How a host parameter's value is moved by the mouse: dragged vertically, shift for fine,
/// double-clicked to its default, clicked to step a choice or give a modulation target its matrix row.
namespace bambi::ui {

/// Where a drag has taken a value: rounded to `step` (none when 0), held to low..high -- or, for a
/// value that is one turn, carried on round from the other end.
double draggedValue(double start, double delta, double step, double low, double high, bool wraps);

/*  How far a drag from `from` to `to` has moved a value, as a share of its whole range: right and up increase,
    and the two add, so a drag that is not quite straight still does what it looks like. Sideways a range is
    `barWidth`, so the end of a bar's fill stays under the pointer; up and down it is `verticalPixels`, the
    finer of the two. No bar (`barWidth` 0) takes `verticalPixels` both ways. `fine` is shift held. */
double dragShare(juce::Point<float> from, juce::Point<float> to, float barWidth, float verticalPixels, bool fine);

/// The bar a value is drawn on, and the values at its two ends: a click on it puts the value where it was
/// clicked, and a sideways drag moves at its width. An empty area is a value with no bar.
struct ValueBar {
    juce::Rectangle<float> area;
    double low{0.0}, high{1.0};
};

/// Where a press on `bar` sets its value: the bar with a margin above and below, so a thin bar needs no aim.
bool onBar(const ValueBar& bar, juce::Point<float> at);
/// The value a press at `x` on `bar` puts there.
double valueOnBar(const ValueBar& bar, float x);

/// What a drag keeps between its press and its moves. Owned by whoever registers the area.
struct HostDrag {
    float start{0.0f};          ///< the value at the press, normalised
    juce::Point<float> from{};  ///< where the press was
};

/// Register the whole gesture on `area`. `stepped` steps on a click; `addsRow` opens the target's matrix row on
/// a click; `bar`, when there is one, takes a click on it to set the value there instead, and scales a sideways
/// drag to its width. `afterDefault` runs after a double-click has put the default back.
void addHostDrag(HitArea& on, PatchModel& model, HostDrag& scratch, juce::Rectangle<float> area, ParamId id,
                 bool stepped, bool addsRow, std::function<void()> afterDefault = {}, juce::Rectangle<float> bar = {});

}  // namespace bambi::ui
