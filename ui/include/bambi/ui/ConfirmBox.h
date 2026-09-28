// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

#include "bambi/ui/HitArea.h"

namespace bambi::ui {

/*  A question over the whole window, for the one thing that cannot be undone. The window dims, the
    box says what is about to happen, and nothing under it takes a press until it is answered. Escape,
    or a press outside the box, is "no". */
class ConfirmBox final : public HitArea {
public:
    ConfirmBox();

    void ask(juce::String question, juce::String note, juce::String yes, std::function<void()> confirmed);
    /// Runs after either answer: whoever asked takes the keyboard back.
    std::function<void()> answered;
    void answer(bool yes);
    bool asking() const { return isVisible(); }
    juce::String question() const { return question_; }

    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;
    /// Where the two buttons were last drawn. For checks.
    juce::Rectangle<float> yesArea() const { return yes_; }
    juce::Rectangle<float> noArea() const { return no_; }

private:
    juce::String question_, note_, yesLabel_;
    std::function<void()> confirmed_;
    juce::Rectangle<float> yes_, no_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConfirmBox)
};

}  // namespace bambi::ui
