// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/editor/EffectControls.h"
#include "bambi/ui/EditorFrame.h"

/*  One frame of a field effect's window: the two probe views and the instance list round
 *  `ui::EditorFrame`.
 */
namespace bambi::editor {

struct EffectFrame {
    EffectControls& controls;
    ui::ProbeView& globe;
    ui::ProbeView& equirect;
    ui::EditorFrame parts;

    void tick(const host::LinkStatus& link, float outputPeak, int order, float dspPercent);
};

}  // namespace bambi::editor
