// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

/*  The suite's installed plugins, loaded as the host loads them: its own VST3 bundles only -- never a scan of the
 *  machine -- each laid out for the part it plays, the whole layout asked for in one request, then prepared, since a
 *  plugin works its order out in prepareToPlay and plays silence until it has. One order for every plugin.
 *
 *    an Encoder   a stereo track in, a stereo sidechain, the field out
 *    an effect    the field in, a stereo sidechain, the field out (Echo, Reverb)
 *    the Decoder  the field in, `outputs` channels out
 *
 *  On the message thread; loading takes a moment per plugin.
 */
namespace bambi::livehost {

enum class Role { Encoder, Effect, Decoder };

struct Loaded {
    std::unique_ptr<juce::AudioPluginInstance> plugin;  ///< none when it could not be loaded or laid out
    juce::String status;                                ///< what was loaded and how, or why not
};

/// `name` as installed, "bambi Encoder". `outputs` is the Decoder's; the others ignore it.
Loaded loadInstalled(const juce::String& name, Role role, int order, double rate, int maxBlock, int outputs = 2);

/// The layout an instance holds, as "in 2 · sidechain 2 · out 16".
juce::String describe(const juce::AudioProcessor::BusesLayout& layout);

}  // namespace bambi::livehost
