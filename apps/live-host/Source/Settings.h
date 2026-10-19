// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include "bambi/live/io.hpp"

/*  Where the host plays and hears on this computer, kept beside the suite's presets in the user's Application
 *  Support as live-host.json. Without the file, no device is chosen and the first Decoder's headphones go back to
 *  Live, on the bambi track. Message thread.
 */
namespace bambi::livehost {

juce::File audioSettingsFile();
live::AudioSettings loadAudioSettings();
/// False when the file could not be written.
bool saveAudioSettings(const live::AudioSettings& settings);

}  // namespace bambi::livehost
