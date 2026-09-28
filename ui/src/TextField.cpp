// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/TextField.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

TextField::TextField(int longest) {
    namespace colour = theme::colour;
    setFont(font(theme::type::body));
    setInputRestrictions(longest);
    setSelectAllWhenFocused(true);
    setIndents(static_cast<int>(theme::settings::fieldPadX), 0);
    setJustification(juce::Justification::centredLeft);
    setColour(juce::TextEditor::backgroundColourId, colour::panel);
    setColour(juce::TextEditor::textColourId, colour::text);
    setColour(juce::TextEditor::highlightColourId, colour::highlight);
    setColour(juce::TextEditor::highlightedTextColourId, colour::text);
    setColour(juce::TextEditor::outlineColourId, colour::rule);
    setColour(juce::TextEditor::focusedOutlineColourId, colour::ruleStrong);
    setColour(juce::TextEditor::shadowColourId,
              colour::panel.withAlpha(0.0f));  // no inner shadow: nothing here has one
    setColour(juce::CaretComponent::caretColourId, colour::text);
    setPopupMenuEnabled(false);  // JUCE's own menu is rounded, shadowed and dark: not this window's
}

}  // namespace bambi::ui
