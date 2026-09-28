// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Controls.h"
#include "bambi/ui/RegionEditor.h"

namespace bambi::ui {

/*  Reverb's right panel: room, send, return.
 *
 *  The frame is the shared one; what is here is which three tabs, and what is in them.
 *
 *  Room holds everything about the engine and its I/O -- the six rooms as a row, output, input --
 *  the same shape as Echo's `taps` tab. Send and return are a tab each, holding the shared region
 *  editor, as Echo's send is its own tab. Ducking has no tab: it is a row in the matrix.
 */
class ReverbPanel final : public ParameterPage {
public:
    explicit ReverbPanel(ReverbControlState& state);

    void paint(juce::Graphics& g) override;

    /// Show the send or the return, as a click on the segment does. For tools.
    void showSlot(int which);
    /// Touch a parameter's value box the way the mouse does. False when it is not in view.
    bool touchParameter(bambi::ParamId id);
    /// Where a room's entry was last drawn, for a check that clicks it. Empty until painted.
    juce::Rectangle<float> roomButton(int index) const;

private:
    void paintRoom(juce::Graphics& g, juce::Rectangle<float> content);
    float paintRooms(juce::Graphics& g, juce::Rectangle<float> content, float y);
    void paintRegions(juce::Graphics& g, juce::Rectangle<float> content, int slot);

    ReverbControlState& state_;
    std::array<juce::Rectangle<float>, 6> rooms_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbPanel)
};

}  // namespace bambi::ui
