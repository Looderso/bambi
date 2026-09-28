// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Tooltips.h"

#include <cmath>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

Tooltips::Tooltips(juce::Component& editor) : juce::TooltipWindow(&editor, theme::tooltip::delayMs) {
    setLookAndFeel(&look_);
}

Tooltips::~Tooltips() { setLookAndFeel(nullptr); }

juce::Rectangle<int> Tooltips::Look::getTooltipBounds(const juce::String& text, juce::Point<int> pointer,
                                                      juce::Rectangle<int> area) {
    const juce::Font f = font(theme::type::body);
    const auto w = static_cast<int>(std::ceil(textWidth(f, text) + 2.0f * theme::tooltip::padX));
    const auto h = static_cast<int>(std::ceil(f.getHeight() + 2.0f * theme::tooltip::padY));
    const juce::Rectangle<int> at{pointer.x - w / 2, pointer.y + static_cast<int>(theme::tooltip::below), w, h};
    return at.constrainedWithin(area);
}

void Tooltips::Look::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
    const juce::Rectangle<float> box{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    g.setColour(theme::colour::panel);
    g.fillRect(box);
    g.setColour(theme::colour::ruleStrong);
    g.drawRect(box, theme::stroke::rule);
    g.setColour(theme::colour::text);
    g.setFont(font(theme::type::body));
    g.drawText(text, box, juce::Justification::centred, false);
}

}  // namespace bambi::ui
