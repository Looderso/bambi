// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The orientation strip, where the effects have their matrix. Its tabs are what tracks the head (none, OSC,
 *  AirPods, a phone), then the tracker's word, recentre and the tracker's setup as a temporary tab; under them the
 *  scene's turn as parameters -- yaw, pitch and roll with the tracker's sign on each axis in the name line, greyed
 *  without a tracker, their rates under them -- each axis's own turn as the live mark. The scene's turn is heard by every
 *  output, so the strip is always on screen.
 */
class HeadStrip final : public ParameterPage {
public:
    explicit HeadStrip(DecoderControlState& state);
    void paint(juce::Graphics& g) override;
    /// Each angle tile's live mark is its own axis's turn: the slider plus what its rate has turned, never the tracker.
    bool liveValue(ParamId id, double& out) const override;
    /// A yaw, pitch or roll double-clicked back to zero clears what its rate had turned.
    void onDefaultRestored(ParamId id) override;
    /// The rate the strip shows, in messages a second; computed by the editor's frame.
    void setTrackerRate(float hz) { rateHz_ = hz; }

private:
    DecoderControlState& state_;
    float rateHz_{0.0f};
    juce::Rectangle<float> recentre_, setup_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeadStrip)
};

}  // namespace bambi::ui
