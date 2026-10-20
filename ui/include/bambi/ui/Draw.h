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
/// The height of one line of a note or hint in the label face.
inline float noteLineHeight() { return theme::type::label * theme::controls::noticeLineHeight; }

inline juce::Font smallFont() { return font(theme::type::small, theme::type::trackingLabel); }

inline juce::Font wordmarkFont() {
    return juce::Font(juce::FontOptions(theme::type::family, "Medium", theme::type::wordmark)
                          .withKerningFactor(theme::type::trackingWordmark));
}

inline float textWidth(const juce::Font& f, const juce::String& s) {
    return juce::GlyphArrangement::getStringWidth(f, s);
}

/// `s` cut from the end to fit `width` with an ellipsis, `keep` always after it whole: a name that does not fit
/// keeps its modified mark.
inline juce::String elided(const juce::Font& f, const juce::String& s, float width, const juce::String& keep = {}) {
    if (textWidth(f, s + keep) <= width) return s + keep;
    const juce::String dots = juce::String::fromUTF8("\xe2\x80\xa6");
    for (int n = s.length() - 1; n > 0; --n) {
        const auto cut = s.substring(0, n).trimEnd() + dots + keep;
        if (textWidth(f, cut) <= width) return cut;
    }
    return dots + keep;
}

inline void text(juce::Graphics& g, const juce::String& s, const juce::Font& f, juce::Colour c,
                 juce::Rectangle<float> box, juce::Justification j = juce::Justification::centredLeft) {
    g.setColour(c);
    g.setFont(f);
    g.drawText(s, box, j, false);
}

/// One label of a switch: the chosen one on an ink block, `block`, as an open tab stands; the others plain. The
/// header's instance switch draws its labels with this, and so does the live host's column of views.
inline void switchLabel(juce::Graphics& g, const juce::String& s, juce::Rectangle<float> box,
                        juce::Rectangle<float> block, bool chosen,
                        juce::Justification j = juce::Justification::centredLeft) {
    if (chosen) {
        g.setColour(theme::colour::chosen);
        g.fillRect(block);
    }
    text(g, s, labelFont(), chosen ? theme::colour::textInverse : theme::colour::text, box, j);
}

/// `path` in dashes, `dash` the on and off lengths, drawn in the current colour with `stroke`'s weight and ends.
inline void strokeDashed(juce::Graphics& g, const juce::Path& path, const float (&dash)[2],
                         const juce::PathStrokeType& stroke) {
    juce::Path dashes;
    stroke.createDashedStroke(dashes, path, dash, 2);
    g.fillPath(dashes);
}

/// The dot that parts two facts on one line: "16 out · 8 in".
inline const juce::String kSeparator = juce::String::fromUTF8(" \xc2\xb7 ");

/// A box's frame, solid or dashed as what the live host plays outside the field, or keeps for itself, is.
inline void drawFrame(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c, float weight, bool dashed) {
    g.setColour(c);
    if (!dashed) {
        g.drawRect(box, weight);
        return;
    }
    juce::Path p;
    p.addRectangle(box);
    strokeDashed(g, p, theme::stroke::outsideDash, juce::PathStrokeType(weight));
}

inline void ruleH(juce::Graphics& g, float x0, float x1, float y, juce::Colour c) {
    g.setColour(c);
    g.fillRect(x0, y, x1 - x0, theme::stroke::rule);
}

inline void ruleV(juce::Graphics& g, float x, float y0, float y1, juce::Colour c) {
    g.setColour(c);
    g.fillRect(x, y0, theme::stroke::rule, y1 - y0);
}

/// A mark of what is happening now, ringed in the ground so its grey reads over any fill beneath it.
inline void nowMark(juce::Graphics& g, juce::Rectangle<float> mark, juce::Colour c) {
    g.setColour(theme::colour::ground);
    g.fillRect(mark.expanded(theme::stroke::liveHalo));
    g.setColour(c);
    g.fillRect(mark);
}

/// A negative number with a real minus sign, as the canvas sets it.
inline juce::String signedNumber(double value, int decimals) {
    //  juce::String(double, 0) means "a default precision", not "none": whole numbers are rounded here.
    const auto body =
        decimals > 0 ? juce::String(std::abs(value), decimals) : juce::String(juce::roundToInt(std::abs(value)));
    return value < 0.0 && body.getDoubleValue() > 0.0 ? juce::String::fromUTF8("\xe2\x88\x92") + body : body;
}

}  // namespace bambi::ui
