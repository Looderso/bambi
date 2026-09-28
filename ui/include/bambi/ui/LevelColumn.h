// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/HostDrag.h"
#include "bambi/ui/PatchModel.h"
#include "bambi/ui/Theme.h"

/*  The level column, down the right edge of every plugin's window: what leaves the plugin, and the
 *  levels that decide it. Outside the tabs, because level is the one reading never to hide.
 *
 *  Each level is a number moved exactly as a tile's value is -- dragged vertically over its own range,
 *  shift for fine, double-clicked to its default, clicked for a matrix row: one statement of that, in
 *  `HostDrag.h` -- stacked above one meter of the output. The encoder has its gain; an effect has its
 *  dry and its wet. Goes through the patch model, so it edits whichever instance is selected like
 *  every other control.
 */
namespace bambi::ui {

class LevelColumn final : public HitArea {
public:
    struct Level {
        ParamId id{kNoParamId};
        juce::String name;  ///< "gain", "dry", "wet": drawn under the number, with its unit
    };

    LevelColumn(PatchModel& model, std::vector<Level> levels);

    /// Once a frame: the output's peak, linear. Falls and holds as a meter does, and repaints.
    void update(float peakLinear, double dtSeconds);

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;

    /// Where a level's number was last drawn, for a check that drags it. Empty for one it has not.
    juce::Rectangle<float> valueArea(ParamId id) const;

private:
    PatchModel& model_;
    std::vector<Level> levels_;
    std::vector<juce::Rectangle<float>> areas_;
    double levelDb_{theme::meter::silenceDb}, holdDb_{theme::meter::silenceDb}, holdLeft_{0.0};
    HostDrag drag_;  ///< a level's drag, between its press and its moves

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelColumn)
};

}  // namespace bambi::ui
