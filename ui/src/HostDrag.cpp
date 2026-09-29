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

double dragShare(juce::Point<float> from, juce::Point<float> to, float barWidth, float verticalPixels, bool fine) {
    const auto across = barWidth > 0.0f ? barWidth : verticalPixels;
    const auto share = (to.x - from.x) / across + (from.y - to.y) / verticalPixels;
    return static_cast<double>(share * (fine ? theme::controls::fineDrag : 1.0f));
}

bool onBar(const ValueBar& bar, juce::Point<float> at) {
    return !bar.area.isEmpty() && bar.area.expanded(0.0f, theme::controls::barHitMargin).contains(at);
}

double valueOnBar(const ValueBar& bar, float x) {
    const auto share = std::clamp((x - bar.area.getX()) / bar.area.getWidth(), 0.0f, 1.0f);
    return bar.low + static_cast<double>(share) * (bar.high - bar.low);
}

void addHostDrag(HitArea& on, PatchModel& model, HostDrag& scratch, juce::Rectangle<float> area, ParamId id,
                 bool stepped, bool addsRow, std::function<void()> afterDefault, juce::Rectangle<float> barArea) {
    namespace ctl = theme::controls;
    const auto& m = model.manifest();
    const ValueBar bar{stepped ? juce::Rectangle<float>{} : barArea, 0.0, 1.0};  // a choice steps; it has no bar

    auto press = [&model, &scratch, id, &m](juce::Point<float> p) {
        scratch.start = static_cast<float>(normalised(m, id, static_cast<double>(model.valueOf(id))));
        scratch.from = p;
        model.beginParameter(id);
    };
    auto drag = [&model, &scratch, id, wraps = m.wrapsAt(static_cast<int>(id)), width = bar.area.getWidth()](
                    juce::Point<float> p, bool fine) {
        const auto delta = dragShare(scratch.from, p, width, ctl::dragPixels, fine);
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
    } else {
        //  on the bar a click puts the value there; elsewhere, touching a value gives it a row, and touching
        //  one that has a row already does nothing
        click = [&model, &scratch, id, bar, addsRow] {
            if (onBar(bar, scratch.from)) {
                model.changeParameter(id, static_cast<float>(valueOnBar(bar, scratch.from.x)));
            } else if (addsRow) {
                model.setProvisionalRow(model.hasRow(id) ? kNoParamId : id);
                model.notifyChanged();
            }
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
