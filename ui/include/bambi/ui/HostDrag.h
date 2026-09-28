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

/// What a drag keeps between its press and its moves. Owned by whoever registers the area.
struct HostDrag {
    float start{0.0f};  ///< the value at the press, normalised
    float y{0.0f};      ///< where the press was
};

/// Register the whole gesture on `area`. `stepped` steps on a click; `addsRow` opens the target's
/// matrix row on a click; `afterDefault` runs after a double-click has put the default back.
void addHostDrag(HitArea& on, PatchModel& model, HostDrag& scratch, juce::Rectangle<float> area, ParamId id,
                 bool stepped, bool addsRow, std::function<void()> afterDefault = {});

}  // namespace bambi::ui
