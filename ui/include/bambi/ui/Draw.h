// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <juce_graphics/juce_graphics.h>

#include "bambi/ui/Theme.h"

/*  Small drawing helpers over Theme.h: fonts at a named size and tracking, text in a box, and the
    rules that carry the structure. Every value still comes from the theme. */
namespace bambi::ui {

inline juce::Font font(float size, float trackingEm = 0.0f) {
    return juce::Font(juce::FontOptions(theme::type::family, size, juce::Font::plain).withKerningFactor(trackingEm));
}

inline juce::Font labelFont() { return font(theme::type::label, theme::type::trackingLabel); }

inline juce::Font smallFont() { return font(theme::type::small, theme::type::trackingLabel); }

inline juce::Font wordmarkFont() {
    return juce::Font(juce::FontOptions(theme::type::family, "Medium", theme::type::wordmark)
                          .withKerningFactor(theme::type::trackingWordmark));
}

inline float textWidth(const juce::Font& f, const juce::String& s) {
    return juce::GlyphArrangement::getStringWidth(f, s);
}

inline void text(juce::Graphics& g, const juce::String& s, const juce::Font& f, juce::Colour c,
                 juce::Rectangle<float> box, juce::Justification j = juce::Justification::centredLeft) {
    g.setColour(c);
    g.setFont(f);
    g.drawText(s, box, j, false);
}

inline void ruleH(juce::Graphics& g, float x0, float x1, float y, juce::Colour c) {
    g.setColour(c);
    g.fillRect(x0, y, x1 - x0, theme::stroke::rule);
}

inline void ruleV(juce::Graphics& g, float x, float y0, float y1, juce::Colour c) {
    g.setColour(c);
    g.fillRect(x, y0, theme::stroke::rule, y1 - y0);
}

/// A negative number with a real minus sign, as the canvas sets it.
inline juce::String signedNumber(double value, int decimals) {
    //  juce::String(double, 0) means "a default precision", not "none": whole numbers are rounded here.
    const auto body =
        decimals > 0 ? juce::String(std::abs(value), decimals) : juce::String(juce::roundToInt(std::abs(value)));
    return value < 0.0 && body.getDoubleValue() > 0.0 ? juce::String::fromUTF8("\xe2\x88\x92") + body : body;
}

}  // namespace bambi::ui
