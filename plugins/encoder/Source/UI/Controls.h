// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <string_view>
#include <vector>

#include "PluginProcessor.h"
#include "bambi/editor/PatchControls.h"
#include "bambi/mod/matrix.hpp"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/MatrixModel.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

/*  What the encoder's matrix and parameter tabs show and edit together. Everything that is not the
    encoder's -- the patch copy and its refresh, the target, the source tab -- is
    `editor::PatchControls`. */
struct ControlState final : public bambi::editor::PatchControls {
    explicit ControlState(BambiEncoderProcessor& p);

    //  ---- the encoder's answers where they differ from every plugin's -------------------------
    /*  The encoder's region is a source: its value is `valueAt` where the source is, one step ago.
        An effect has no source, and says no. */
    bool regionIsSource() const override { return true; }

    /*  Another instance is selected in the scene, which only the encoder has; the shell keeps the
        label and the way back. */
    juce::String anotherLabel() const override { return selectedLabel; }
    void selectSelfInstance() override {
        if (selectSelf) selectSelf();
    }

    BambiEncoderProcessor& processor;  ///< this instance, whichever one the controls show
    int renderedOrder() const final {
        return std::clamp(processor.diagnostics().order.load(), 1, bambi::kRegionWeightsOrder);
    }
    juce::String selectedLabel;
    std::function<void()> selectSelf;  ///< select this instance again
};

}  // namespace bambi::ui
