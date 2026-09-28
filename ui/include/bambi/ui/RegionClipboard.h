// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>

#include "bambi/mod/regionclip.hpp"

/// The system clipboard, as the region editor's copy and paste use it. Replaceable, so a check that
/// copies a region does not overwrite whatever is really on it; read sparingly -- twice a second at most.
namespace bambi::ui {

struct TextClipboard {
    std::function<juce::String()> read;
    std::function<void(const juce::String&)> write;
};

/// The one in use: the system's, until something puts its own here. Message thread only.
TextClipboard& textClipboard();

/// The region the clipboard holds, or nothing -- text that is not a current descriptor is nothing.
std::optional<RegionClip> regionOnClipboard();

/// Put a region on it, and forget what was last read so the next look sees this.
void copyToClipboard(const RegionClip& clip);

/// Forget what was last read, so a check that changes the clipboard under the editor is seen without waiting.
void clipboardChanged();

}  // namespace bambi::ui
