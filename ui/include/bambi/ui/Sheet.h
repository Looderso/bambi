// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

#include "bambi/ui/HitArea.h"

namespace bambi::ui {

/*  A sheet over the whole window: the window dims, and nothing under it takes a press or a key while it is open.
    Escape, or a press outside its box, dismisses it. What it holds is its subclass's, painted in the box it draws.
    Shared by every one -- a question, a host's settings -- so each dims, closes and gives the keyboard back alike. */
class Sheet : public HitArea {
public:
    Sheet();

    /// Runs after it closes, however it was closed: whoever opened it takes the keyboard back.
    std::function<void()> closed;
    bool isOpen() const { return isVisible(); }
    void open();
    void close();

    bool keyPressed(const juce::KeyPress& key) override;

protected:
    /// What escape and a press outside the box do: close it.
    virtual void dismiss() { close(); }
    /// First in a subclass's paint: the window dimmed, a press outside `box` dismissing and one on it doing
    /// nothing, and the box's ground and frame.
    void paintSheet(juce::Graphics& g, juce::Rectangle<float> box);
};

}  // namespace bambi::ui
