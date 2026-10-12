// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The orientation strip, where the effects have their matrix. Its tabs are what tracks the head (none, OSC,
 *  AirPods, a phone), the tracker's word beside them; under them whether its orientation is undone as a head or
 *  turns the scene, recentre, the tracker's setup as a temporary tab, static; then the scene's turn as parameters
 *  -- yaw, pitch and roll with the tracker's sign on each axis in the name line, their rates under them -- the
 *  turn in force as the live mark. The scene's turn is heard by every output, so the strip is always on screen.
 */
class HeadStrip final : public ParameterPage {
public:
    explicit HeadStrip(DecoderControlState& state);
    void paint(juce::Graphics& g) override;
    /// The scene's turn in force marks the three angle tiles: slider, rate and, when it turns the scene, tracker.
    bool liveValue(ParamId id, double& out) const override;
    /// The rate the strip shows, in messages a second; computed by the editor's frame.
    void setTrackerRate(float hz) { rateHz_ = hz; }

    juce::Rectangle<float> recentreArea() const { return recentre_; }
    juce::Rectangle<float> setupArea() const { return setup_; }
    juce::Rectangle<float> sourceCell(int index) const {
        return index >= 0 && index < static_cast<int>(sourceCells_.size())
                   ? sourceCells_[static_cast<std::size_t>(index)]
                   : juce::Rectangle<float>{};
    }

private:
    DecoderControlState& state_;
    float rateHz_{0.0f};
    juce::Rectangle<float> recentre_, setup_, static_;
    std::vector<juce::Rectangle<float>> sourceCells_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeadStrip)
};

}  // namespace bambi::ui
