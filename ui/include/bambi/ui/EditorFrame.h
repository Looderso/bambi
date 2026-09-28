// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/ui/Header.h"
#include "bambi/ui/LevelColumn.h"
#include "bambi/ui/MatrixView.h"
#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/SettingsPage.h"
#include "bambi/ui/StatusFooter.h"

/*  One frame of every plugin's window: what the timer does to the parts all three share -- header,
 *  matrix, page, settings, level column, footer.
 *
 *  A plugin's own views -- a scene, a probe -- are its own to repaint, before or after. What is here
 *  is the part shared by all three, kept in one place so it cannot drift between them.
 */
namespace bambi::ui {

/// Frames a second, and so the `dt` a frame covers: every window's timer runs at this.
inline constexpr int kFrameHz = 30;

struct EditorFrame {
    Header& header;
    MatrixView& matrix;
    ParameterPage& page;
    SettingsPage& settings;
    LevelColumn& levels;
    StatusFooter& footer;

    /// What the plugin knows this frame that the parts do not.
    struct Facts {
        Header::Link link;
        bool canUndo{false}, canRedo{false};
        juce::String undoName, redoName;  ///< what each would revert or reapply, for the tooltips
        bool changed{false};              ///< the patch copy moved: `PatchControls::refresh()`, or a selection
        float outputPeak{0.0f};
        juce::String instance;  ///< the instance shown, for the footer
        int order{1};
        float dspPercent{0.0f};
    };

    /// True when the engine moved something the page draws: a view drawn from it should repaint too.
    bool refresh(const Facts& now);
};

}  // namespace bambi::ui
