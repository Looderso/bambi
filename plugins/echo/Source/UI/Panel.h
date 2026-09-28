// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Controls.h"
#include "Strip.h"
#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/RegionEditor.h"

namespace bambi::ui {

/*  Echo's right panel: taps and regions.
 *
 *  The frame is the shared one; what is here is which tabs, and what is in them. Ducking has no
 *  tab: it is a row in the matrix.
 *
 *  Taps is the one with a shape of its own. A tap selector -- four rows, each carrying a tap's
 *  on/off at one end and its level at the other -- and under it a tab choosing timing, axis and
 *  every pass, drawing only that category of the selected tap. The row says which tap, the tab
 *  says what, so a delay is built by setting four periods, then four axes, then four per-pass
 *  behaviours. Switching tab never moves the selection and selecting never moves the tab.
 *
 *  The selector doubles as the picture: each row's track carries the tap's pass marks on the one
 *  shared time axis, under the on/off and level at its ends.
 */
class EchoPanel final : public ParameterPage {
public:
    explicit EchoPanel(EchoControlState& state);

    void paint(juce::Graphics& g) override;

    /// Select a tap, as a click on its row does. For tools.
    void selectTap(int index);
    /// Open a category of the selected tap, as a click does. For tools.
    void showCategory(int index);

    /*  Touch a parameter's value box the way the mouse does -- through the painted region and the
        click handler. False when it is not on what is in view. For tools. */
    bool touchParameter(bambi::ParamId id);

    /// Where a tap's row was last drawn, for a check that clicks it. Empty until painted.
    juce::Rectangle<float> tapRow(int index) const;
    /// Where its on/off switch was drawn. It is a switch, not a tile: it has no value box.
    juce::Rectangle<float> tapSwitch(int index) const;

private:
    float paintPattern(juce::Graphics& g, juce::Rectangle<float> content, float y);
    float paintStrip(juce::Graphics& g, juce::Rectangle<float> content, float y);
    void paintTrack(juce::Graphics& g, juce::Rectangle<float> track, const StripContent& content, int tapIndex);
    void paintRuler(juce::Graphics& g, juce::Rectangle<float> ruler, const StripContent& content);
    void paintTaps(juce::Graphics& g, juce::Rectangle<float> content);
    float paintTiming(juce::Graphics& g, juce::Rectangle<float> content, float y);
    float paintAxis(juce::Graphics& g, juce::Rectangle<float> content, float y);
    float paintEveryPass(juce::Graphics& g, juce::Rectangle<float> content, float y);
    /// wet and dry: the plugin's, not a tap's, so the tap tabs do not reach them
    void paintRegions(juce::Graphics& g, juce::Rectangle<float> content);

    EchoControlState& state_;
    std::array<juce::Rectangle<float>, bambi::kEchoTaps> tapRows_{}, switchAreas_{};
    double span_{1.0};                  ///< the shared axis, as the last paint computed it. For tools.
    juce::Rectangle<float> stripArea_;  ///< where the last paint drew the strip
    juce::Rectangle<float> enginePicture() const override { return stripArea_; }

public:
    /// The span the strip's four rows share, in seconds. For tools.
    double spanSeconds() const { return span_; }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoPanel)
};

}  // namespace bambi::ui
