// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The encoder's source settings are `ui/SourceSettings` -- shared by all three plugins, keyed by
//  name. Only the one line that says which region slot is the encoder's.

#include "bambi/ui/SourceSettings.h"

#include "UI/ParameterTabs.h"

namespace bambi::ui {

void ParameterTabs::paintSourceSettings(juce::Graphics& g, juce::Rectangle<float> content) {
    bambi::ui::paintSourceSettings(*this, g, content, state_.source, state_.regionSlot());
}

}  // namespace bambi::ui
