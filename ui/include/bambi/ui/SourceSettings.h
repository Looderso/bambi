// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/RegionEditor.h"

/// A source's own settings: one temporary tab whose content depends on the kind of source.
namespace bambi::ui {

/// Draws the settings for source `slot` into `content`; `region` picks the region slot when the source is the region.
void paintSourceSettings(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, int slot,
                         const RegionSlot& region);

}  // namespace bambi::ui
