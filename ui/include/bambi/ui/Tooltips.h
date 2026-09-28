// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace bambi::ui {

/*  The one tooltip window an editor owns: a child of the editor, drawn in the theme. A component that
    has something to say implements juce::TooltipClient. */
class Tooltips final : public juce::TooltipWindow {
public:
    explicit Tooltips(juce::Component& editor);
    ~Tooltips() override;

private:
    struct Look final : juce::LookAndFeel_V4 {
        juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> pointer,
                                              juce::Rectangle<int> area) override;
        void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override;
    };
    Look look_;
};

}  // namespace bambi::ui
