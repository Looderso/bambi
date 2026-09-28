// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/MatrixModel.h"

namespace bambi::ui {

/*  The modulation matrix, shared by every plugin: the rows are `matrixTargets(mod(), patch())`, so the
    same component serves the encoder, Echo and Reverb. Sits under the scene -- never a tab. Three tabs
    over one list of target rows, six source columns each.

      - a column's blue bar is its amount, an automatable parameter: drag it
      - a cell's bar is depth, which is state: drag it, click to set or clear, double-click to clear
      - a violet dot marks a target with depth on another tab
      - the dashed row is provisional: a parameter name was clicked, and any depth commits it
*/
class MatrixView final : public HitArea {
public:
    explicit MatrixView(MatrixModel& model);
    void paint(juce::Graphics& g) override;

    /// Repaint only the column heads, where the live source values move.
    void repaintHeader();
    /// How many times the column heads were asked to repaint. For checks.
    int headerRepaintsAsked() const { return headerRepaints_; }

    /// Which tabs this plugin's matrix offers, in the order they are drawn: the region's only where
    /// it is a source. Walked by both the paint and a check, so the two cannot disagree.
    std::vector<MatrixTab> tabsOffered() const;

    /// How many source columns the last paint drew: one on the region's tab, six on the others.
    int columnsDrawn() const { return columnsDrawn_; }

    /// Where a tab's name was last drawn, or empty for one this plugin does not offer.
    juce::Rectangle<float> tabArea(MatrixTab tab) const;

    /// Where a source column's head was last drawn. Empty for a column the current tab does not have.
    juce::Rectangle<float> columnHeadArea(int column) const;

private:
    int headerRepaints_{0};
    void paintFooter(juce::Graphics& g);

    MatrixModel& model_;
    int columnsDrawn_{0};
    std::array<juce::Rectangle<float>, 4> tabAreas_{};
    std::array<juce::Rectangle<float>, kSourcesPerTab> columnHeads_{};
    float dragStart_{0.0f};
    float dragY_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MatrixView)
};

}  // namespace bambi::ui
