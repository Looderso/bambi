// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/PatchModel.h"

/// The footer under the right-hand panel, the same in every plugin: what is true of the whole
/// instance and is not a level. On the left, the switches that belong to no page because they
/// govern all of them -- `rates continue` everywhere, and a plugin's own beside it. On the right,
/// the readout: whose controls these are, the order the host settled on, and what this plugin's
/// audio costs.
namespace bambi::ui {

class StatusFooter final : public HitArea {
public:
    struct Switch {
        ParamId id{kNoParamId};
        juce::String label;  ///< what it says when ON: "rates continue", "realistic"
    };

    StatusFooter(PatchModel& model, std::vector<Switch> switches);

    /// Once a frame. Repaints only when what it would draw has changed. `dspPercent` is the audio
    /// thread's share; drawing the window itself is the main thread's cost, shown in a host's meter.
    void setStatus(const juce::String& instance, int order, float dspPercent);

    void paint(juce::Graphics& g) override;

    /// Where a switch was last drawn, for a check that clicks it. Empty for one it does not have.
    juce::Rectangle<float> switchArea(ParamId id) const;
    const juce::String& readout() const { return readout_; }
    /// What the last paint drew: the readout, or without the instance's name where it would not fit.
    const juce::String& drawnReadout() const { return drawnReadout_; }
    bool readoutFits() const { return readoutFits_; }  ///< the last paint drew it whole

private:
    PatchModel& model_;
    std::vector<Switch> switches_;
    std::vector<juce::Rectangle<float>> areas_;
    std::vector<bool> drawn_;  ///< each switch as last painted, to know when to repaint
    juce::String readout_;
    juce::String shortReadout_;  ///< without the instance's name, drawn when the whole would not fit
    juce::String drawnReadout_;
    bool readoutFits_{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusFooter)
};

}  // namespace bambi::ui
