// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/StatusFooter.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace sc = theme::scene;
}  // namespace

StatusFooter::StatusFooter(PatchModel& model, std::vector<Switch> switches)
    : model_(model), switches_(std::move(switches)), areas_(switches_.size()), drawn_(switches_.size(), false) {}

void StatusFooter::setStatus(const juce::String& instance, int order, float dspPercent) {
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    auto next = "order " + (order >= 0 ? juce::String(order) : juce::String("-")) + dot + juce::String(dspPercent, 2) +
                " % dsp";
    shortReadout_ = next;
    if (instance.isNotEmpty()) next = instance + dot + next;

    bool moved = next != readout_;
    for (std::size_t i = 0; i < switches_.size(); ++i)
        moved = moved || (model_.valueOf(switches_[i].id) > 0.5f) != drawn_[i];
    readout_ = std::move(next);
    if (moved) repaint();
}

juce::Rectangle<float> StatusFooter::switchArea(ParamId id) const {
    for (std::size_t i = 0; i < switches_.size(); ++i)
        if (switches_[i].id == id) return areas_[i];
    return {};
}

void StatusFooter::paint(juce::Graphics& g) {
    clearRegions();
    g.fillAll(colour::ground);
    const auto bounds = getLocalBounds().toFloat();
    const auto inset = static_cast<float>(theme::space::inset);
    ruleH(g, 0.0f, bounds.getWidth(), 0.0f, colour::rule);  // under the window's own rule, which is drawn over it

    //  the switches, from the left: a square and what it says when it is on
    auto x = inset;
    for (std::size_t i = 0; i < switches_.size(); ++i) {
        const auto id = switches_[i].id;
        const bool on = model_.valueOf(id) > 0.5f;
        drawn_[i] = on;
        const auto wide = sceneToggleWidth(switches_[i].label);
        const auto toggle = paintSceneToggle(g, bounds, x + wide, switches_[i].label, on);
        areas_[i] = toggle.hit;
        //  as it stands when clicked, not as it stood when this was painted
        addClickArea(toggle.hit, [this, id] {
            model_.changeParameter(id, model_.valueOf(id) > 0.5f ? 0.0f : 1.0f);
            model_.notifyChanged();
        });
        x += wide + sc::toggleGroupGap;
    }

    const auto room = bounds.reduced(inset, 0.0f).withLeft(x);
    drawnReadout_ = textWidth(labelFont(), readout_) <= room.getWidth() ? readout_ : shortReadout_;
    readoutFits_ = textWidth(labelFont(), drawnReadout_) <= room.getWidth();
    text(g, drawnReadout_, labelFont(), colour::label, room, juce::Justification::centredRight);
}

}  // namespace bambi::ui
