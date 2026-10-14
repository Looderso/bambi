// SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugins.h"

namespace bambi::livehost {

namespace {

juce::File installed(const juce::String& name) {
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Audio/Plug-Ins/VST3")
        .getChildFile(name + ".vst3");
}

}  // namespace

juce::String describe(const juce::AudioProcessor::BusesLayout& l) {
    juce::StringArray parts;
    for (int i = 0; i < l.inputBuses.size(); ++i)
        parts.add((i == 0 ? "in " : "sidechain ") + juce::String(l.inputBuses[i].size()));
    for (int i = 0; i < l.outputBuses.size(); ++i) parts.add("out " + juce::String(l.outputBuses[i].size()));
    return parts.joinIntoString(" · ");
}

Loaded loadInstalled(const juce::String& name, Role role, int order, double rate, int maxBlock, int outputs) {
    Loaded out;
    const auto file = installed(name);
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    if (file.exists() && format.fileMightContainThisPluginType(file.getFullPathName()))
        format.findAllTypesForFile(found, file.getFullPathName());
    if (found.isEmpty()) {
        out.status = name + " is not installed at " + file.getFullPathName();
        return out;
    }
    juce::String error;
    out.plugin = format.createInstanceFromDescription(*found[0], rate, maxBlock, error);
    if (out.plugin == nullptr) {
        out.status = name + " would not load: " + error;
        return out;
    }

    const auto field = juce::AudioChannelSet::ambisonic(order);
    auto layout = out.plugin->getBusesLayout();
    auto& ins = layout.inputBuses;
    auto& outs = layout.outputBuses;
    if (ins.size() > 0) ins.getReference(0) = role == Role::Encoder ? juce::AudioChannelSet::stereo() : field;
    if (ins.size() > 1) ins.getReference(1) = juce::AudioChannelSet::stereo();  // the sidechain; the Decoder has none
    if (outs.size() > 0)
        outs.getReference(0) = role == Role::Decoder ? juce::AudioChannelSet::discreteChannels(outputs) : field;
    if (role == Role::Decoder && outputs == 2) outs.getReference(0) = juce::AudioChannelSet::stereo();

    const bool took = out.plugin->setBusesLayout(layout);
    out.plugin->prepareToPlay(rate, maxBlock);
    out.status = out.plugin->getName() + " · VST3 · " + describe(out.plugin->getBusesLayout());
    if (!took) {
        out.status += " -- refused " + describe(layout);
        out.plugin.reset();
    }
    return out;
}

}  // namespace bambi::livehost
