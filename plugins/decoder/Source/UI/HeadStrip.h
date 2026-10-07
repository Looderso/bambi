// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Controls.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The head strip, where the effects have their matrix: what turns the head (none, OSC; AirPods and a phone later),
 *  whether data arrives and how evenly, yaw, pitch and roll as parameters with the tracker's value as the live mark,
 *  recentre, and the tracker's setup as a temporary tab. The head serves every output, so its strip is always
 *  on screen.
 */
class HeadStrip final : public ParameterPage {
public:
    explicit HeadStrip(DecoderControlState& state);
    void paint(juce::Graphics& g) override;
    /// Where the tracker's own value marks a tile while it overrides the parameters.
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
    juce::Rectangle<float> recentre_, setup_, overrides_;
    std::vector<juce::Rectangle<float>> sourceCells_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeadStrip)
};

}  // namespace bambi::ui
