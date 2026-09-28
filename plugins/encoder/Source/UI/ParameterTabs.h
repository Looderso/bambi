// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <initializer_list>
#include <string>

#include "UI/Controls.h"
#include "UI/SceneModel.h"
#include "bambi/ui/ParameterPage.h"

namespace bambi::ui {

/*  The right-hand column: what acts on the path's shape, where the path sits, and the
    moving source itself -- trajectory, transform, source.

    A host parameter is dragged vertically (shift for fine), double-clicked to its default; a choice or
    a switch steps on a click. Clicking the name of a parameter that takes modulation adds a provisional
    row to the matrix -- the assign flow. The trajectory's own settings are state, and change through
    undoable edits.

    A source's settings -- an LFO, an envelope and its trigger, a feature's calibration -- are a fourth,
    temporary page, opened from the source's matrix column (SourceSettings.cpp). */
class ParameterTabs final : public ParameterPage {
public:
    ParameterTabs(ControlState& state, SceneState& scene);
    void paint(juce::Graphics& g) override;

    using ParameterPage::nameArea;
    /// Where the prev and next chevrons were last drawn, likewise.
    juce::Rectangle<float> stepArea(bool forward) const { return sourceStepAreas()[forward ? 1 : 0]; }

    /*  The encoder's answer to what the engine is applying: only the placement has one, since the bus
        publishes the transform the renderer used and a rate's drift is in it. */
    bool liveValue(bambi::ParamId id, double& out) const override;
    /// Whether the last paint found anything that moves on its own. For tools.
    bool hasLiveRegion() const { return !liveRegion().isEmpty(); }
    /// The region that paint registered as moving -- what repaintMoving() comes back for. For tools.
    juce::Rectangle<float> liveBounds() const { return liveRegion(); }

private:
    void paintTrajectory(juce::Graphics& g, juce::Rectangle<float> content);
    float paintNode(juce::Graphics& g, juce::Rectangle<float> content, float y);  ///< the node selected in the scene
    /// The encoder's temporary fourth tab, drawn into the shared frame's bar. Returns the x it ended at.
    void paintTransform(juce::Graphics& g, juce::Rectangle<float> content);
    void paintSource(juce::Graphics& g, juce::Rectangle<float> content);
    /// The width's two ends, drawn as one box: a custom cell in the group, named where it is used.
    void addWidthRangeTile(juce::Graphics& g, juce::Rectangle<float> tile);
    /// An angle back to its default means the angle, not the turn a rate has added.
    void onDefaultRestored(bambi::ParamId id) override;

    //  a source's settings (SourceSettings.cpp)
    void paintSourceSettings(juce::Graphics& g, juce::Rectangle<float> content);

    ControlState& state_;
    SceneState& scene_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterTabs)
};

}  // namespace bambi::ui
