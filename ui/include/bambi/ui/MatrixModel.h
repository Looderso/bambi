// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include "bambi/ui/PatchModel.h"
#include "bambi/ui/RegionEditor.h"

/// What the shared modulation matrix needs beyond the patch itself: which tab is open, which source's
/// settings are on screen, and whether this window is showing another instance.
namespace bambi::ui {

class MatrixModel : public PatchModel {
public:
    virtual MatrixTab currentTab() const = 0;
    virtual void setCurrentTab(MatrixTab t) = 0;

    //  ---- the source columns -------------------------------------------------------------------
    /// That source's settings are the ones on screen, so its column is marked open.
    bool sourceOpen(int slot) const { return openSourceSlot() == slot && sourceTabShown(); }
    virtual void openSourceSettings(int slot) = 0;

    /// True when the region is a source with its own matrix tab, as in the encoder; false where a
    /// region has no direction to read, as in an effect. False by default.
    virtual bool regionIsSource() const { return false; }

    /// Which slot that source is; the encoder's one region is slot 0, `region1`, named `region`.
    virtual RegionSlot regionSlot() const { return {0, "region1", "region"}; }
};

}  // namespace bambi::ui
