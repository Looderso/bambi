// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/HostDrag.h"

#include <algorithm>
#include <cmath>

#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"

namespace bambi::ui {

double draggedValue(double start, double delta, double step, double low, double high, bool wraps) {
    double x = start + delta;
    if (step > 0.0) x = std::round(x / step) * step;
    if (!wraps) return std::clamp(x, low, high);
    const double span = high - low;
    return low + (x - low) - span * std::floor((x - low) / span);
}

void addHostDrag(HitArea& on, PatchModel& model, HostDrag& scratch, juce::Rectangle<float> area, ParamId id,
                 bool stepped, bool addsRow, std::function<void()> afterDefault) {
    namespace ctl = theme::controls;
    const auto& m = model.manifest();

    auto press = [&model, &scratch, id, &m](juce::Point<float> p) {
        scratch.start = static_cast<float>(normalised(m, id, static_cast<double>(model.valueOf(id))));
        scratch.y = p.y;
        model.beginParameter(id);
    };
    auto drag = [&model, &scratch, id, wraps = m.wrapsAt(static_cast<int>(id))](juce::Point<float> p, bool fine) {
        const auto delta = (scratch.y - p.y) / ctl::dragPixels * (fine ? ctl::fineDrag : 1.0f);
        model.setParameter(id, static_cast<float>(draggedValue(scratch.start, delta, 0.0, 0.0, 1.0, wraps)));
    };
    auto release = [&model, id] { model.endParameter(id); };

    std::function<void()> click;
    if (stepped) {
        //  a choice or a switch steps to its next option, and wraps -- it takes no modulation, so a click is free
        click = [&model, id, &m] {
            const auto& d = m[static_cast<int>(id)];
            const auto options = std::max(2, d.type == ParamType::Bool ? 2 : choiceCount(d));
            const auto next = (juce::roundToInt(model.valueOf(id)) + 1) % options;
            model.changeParameter(id, static_cast<float>(next) / static_cast<float>(options - 1));
        };
    } else if (addsRow) {
        //  touching a value gives it a row; touching one that has a row already does nothing
        click = [&model, id] {
            model.setProvisionalRow(model.hasRow(id) ? kNoParamId : id);
            model.notifyChanged();
        };
    }
    auto doubleClick = [&model, id, &m, after = std::move(afterDefault)] {
        model.changeParameter(id,
                              static_cast<float>(normalised(m, id, static_cast<double>(m[static_cast<int>(id)].def))));
        if (after) after();
    };
    on.addGestureArea(area, std::move(press), std::move(drag), std::move(release), std::move(click),
                      std::move(doubleClick));
}

}  // namespace bambi::ui
