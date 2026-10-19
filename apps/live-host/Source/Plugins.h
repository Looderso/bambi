// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <string_view>

#include "bambi/patch/layout.hpp"
#include "bambi/live/field.hpp"
#include "bambi/live/lanes.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/remote/Remote.h"

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

/// `name` as installed, "bambi Path Encoder". `outputs` is the Decoder's; the others ignore it.
Loaded loadInstalled(const juce::String& name, Role role, int order, double rate, int maxBlock, int outputs = 2);

/// `channels` channels as a VST3 plugin can be asked for them: that many distinct speakers, since JUCE passes a
/// VST3 plugin only channels that name a speaker, and a discrete set wider than one names none. Stereo for two;
/// empty past kMaxVst3BusChannels. Which speakers is of no account: the Decoder plays every channel by its index.
juce::AudioChannelSet speakerChannels(int channels);

/// The suite's remote interface on `plugin`, when it speaks the payload `kind` at `version`; null for a plugin that
/// offers none or speaks another. Lives as long as the plugin. Message thread.
remote::IRemote* remoteOf(juce::AudioPluginInstance& plugin, std::string_view kind, int version);
/// What it reports, as JSON text.
std::string remoteReport(remote::IRemote& remote);

/// The parameters an instance of the suite lists, as its manifest has them: an Encoder's, an Echo's or a
/// Reverb's as `effect` says for an insert, a Decoder's.
const ParamManifest& manifestOf(live::Instance instance, live::Effect effect = live::Effect::Echo);

/// The layout an instance holds, as "in 2 · sidechain 2 · out 16".
juce::String describe(const juce::AudioProcessor::BusesLayout& layout);

}  // namespace bambi::livehost
