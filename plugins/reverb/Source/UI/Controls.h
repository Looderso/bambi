// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>

#include "../PluginProcessor.h"
#include "bambi/editor/EffectControls.h"
#include "bambi/ui/ProbeView.h"

namespace bambi::ui {

/*  What Reverb's window shows and edits: a copy of the patch, refreshed every frame, and the editor's
    own view state -- which tab is open, which tap is selected, and the provisional matrix row.

    It is the encoder's `ControlState` in shape and not in substance: the shared `MatrixModel` is what
    both answer, and what differs is only which plugin's manifest and which processor.

    Everything that is not Reverb's -- the patch copy and its refresh, the target, the source tab,
    the instance list, the energy picture -- is `editor::EffectControls`.
*/
struct ReverbControlState final : public bambi::editor::EffectControls {
    explicit ReverbControlState(BambiReverbProcessor& p);

    //  ---- Reverb's own ---------------------------------------------------------------------
    BambiReverbProcessor& processor;
    int renderedOrder() const final { return std::clamp(processor.engineOrder(), 1, bambi::kRegionWeightsOrder); }

    /*  Reverb's answer to the probe: the early reflections a source in that direction would
        produce, each where it arrives from, at its weight. The same component as Echo's ping;
        only the answer differs. */
    void answerProbe(bambi::Vec3 from, bambi::ui::ProbeReply& into) override;

    /// A parameter by key: `param("room.size")`. kNoParamId when this build has no such key.
    bambi::ParamId param(const char* key) const;

    /// A region slot's parameter: `slotParam(1, "yaw")` is `region2.yaw`.
    bambi::ParamId slotParam(int which, const char* field) const;
};

}  // namespace bambi::ui
