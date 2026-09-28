// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>

#include "../PluginProcessor.h"
#include "bambi/editor/EffectControls.h"
#include "bambi/ui/ProbeView.h"

namespace bambi::ui {

/*  What Echo's window shows and edits: a copy of the patch, refreshed every frame, and the editor's
    own view state -- which tab is open, which tap is selected, and the provisional matrix row.

    It is the encoder's `ControlState` in shape and not in substance: the shared `MatrixModel` is
    what both answer, and what differs is only which plugin's manifest and which processor.

    Everything that is not Echo's -- the patch copy and its refresh, the target, the source tab,
    the instance list, the energy picture -- is `editor::EffectControls`.
*/
struct EchoControlState final : public bambi::editor::EffectControls {
    explicit EchoControlState(BambiEchoProcessor& p);

    //  ---- Echo's own -----------------------------------------------------------------------
    BambiEchoProcessor& processor;
    int tap{0};       ///< which of the four the panel shows, 0-3
    int category{0};  ///< timing, axis, every pass

    /// Echo's answer to the probe: every enabled tap's spiral from that direction.
    void answerProbe(bambi::Vec3 from, bambi::ui::ProbeReply& into) override;

    int renderedOrder() const final { return std::clamp(processor.engineOrder(), 1, bambi::kRegionWeightsOrder); }
    /// A tap's parameter, by the key that follows its prefix: `tapParam(2, "level")`.
    bambi::ParamId tapParam(int tapIndex, const char* field) const;
};

}  // namespace bambi::ui
