// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace bambi::ui {

/*  The suite's text field, dressed from the theme: a value's box, in the body's type, with no menu,
    no shadow and no rounding of JUCE's own.

    `onKey` sees a key before the field does and says whether it took it: a list under a search field
    is walked with the arrows while the typing stays in the field. */
class TextField final : public juce::TextEditor {
public:
    explicit TextField(int longest);

    bool keyPressed(const juce::KeyPress& key) override {
        return (onKey && onKey(key)) || juce::TextEditor::keyPressed(key);
    }

    std::function<bool(const juce::KeyPress&)> onKey;
};

}  // namespace bambi::ui
