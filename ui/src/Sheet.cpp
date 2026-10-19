// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Sheet.h"

#include "bambi/ui/Theme.h"

namespace bambi::ui {

Sheet::Sheet() {
    setWantsKeyboardFocus(true);
    setVisible(false);
}

void Sheet::open() {
    setVisible(true);
    toFront(true);
    grabKeyboardFocus();
    repaint();
}

void Sheet::close() {
    if (!isVisible()) return;
    setVisible(false);
    if (closed) closed();
}

bool Sheet::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) dismiss();
    return true;  // nothing under the sheet takes a key either
}

void Sheet::paintSheet(juce::Graphics& g, juce::Rectangle<float> box) {
    clearRegions();
    const auto all = getLocalBounds().toFloat();
    g.fillAll(theme::colour::text.withAlpha(theme::sheet::scrim));
    addClickArea(all, [this] { dismiss(); });
    g.setColour(theme::colour::ground);
    g.fillRect(box);
    g.setColour(theme::colour::ruleStrong);
    g.drawRect(box, theme::stroke::rule);
    addClickArea(box, [] {});  // a press on the sheet itself, between its controls, does nothing
}

}  // namespace bambi::ui
