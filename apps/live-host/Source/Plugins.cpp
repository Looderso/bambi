// SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugins.h"

#include <iterator>
#include <pluginterfaces/vst/ivstcomponent.h>

#include "bambi/decode/params.hpp"
#include "bambi/echo/params.hpp"
#include "bambi/encode/params.hpp"
#include "bambi/link/link.hpp"
#include "bambi/reverb/params.hpp"
#include "bambi/ui/Draw.h"

namespace bambi::livehost {

namespace {

juce::File installed(const juce::String& name) {
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Audio/Plug-Ins/VST3")
        .getChildFile(name + ".vst3");
}

}  // namespace

juce::AudioChannelSet speakerChannels(int channels) {
    using T = juce::AudioChannelSet::ChannelType;
    //  every type JUCE maps to a VST3 speaker, in JUCE's own order, so a set's channel i is the i-th of these
    static constexpr T kSpeakers[] = {T::left,
                                      T::right,
                                      T::centre,
                                      T::LFE,
                                      T::leftSurround,
                                      T::rightSurround,
                                      T::leftCentre,
                                      T::rightCentre,
                                      T::centreSurround,
                                      T::leftSurroundSide,
                                      T::rightSurroundSide,
                                      T::topMiddle,
                                      T::topFrontLeft,
                                      T::topFrontCentre,
                                      T::topFrontRight,
                                      T::topRearLeft,
                                      T::topRearCentre,
                                      T::topRearRight,
                                      T::LFE2,
                                      T::leftSurroundRear,
                                      T::rightSurroundRear,
                                      T::wideLeft,
                                      T::wideRight,
                                      T::ambisonicACN0,
                                      T::ambisonicACN1,
                                      T::ambisonicACN2,
                                      T::ambisonicACN3,
                                      T::topSideLeft,
                                      T::topSideRight,
                                      T::ambisonicACN4,
                                      T::ambisonicACN5,
                                      T::ambisonicACN6,
                                      T::ambisonicACN7,
                                      T::ambisonicACN8,
                                      T::ambisonicACN9,
                                      T::ambisonicACN10,
                                      T::ambisonicACN11,
                                      T::ambisonicACN12,
                                      T::ambisonicACN13,
                                      T::ambisonicACN14,
                                      T::ambisonicACN15,
                                      T::ambisonicACN16,
                                      T::ambisonicACN17,
                                      T::ambisonicACN18,
                                      T::ambisonicACN19,
                                      T::ambisonicACN20,
                                      T::ambisonicACN21,
                                      T::ambisonicACN22,
                                      T::ambisonicACN23,
                                      T::ambisonicACN24,
                                      T::bottomFrontLeft,
                                      T::bottomFrontCentre,
                                      T::bottomFrontRight,
                                      T::proximityLeft,
                                      T::proximityRight,
                                      T::bottomSideLeft,
                                      T::bottomSideRight,
                                      T::bottomRearLeft,
                                      T::bottomRearCentre,
                                      T::bottomRearRight,
                                      T::discreteChannel0};
    static_assert(std::size(kSpeakers) == kMaxVst3BusChannels);
    if (channels < 1 || channels > kMaxVst3BusChannels) return {};
    juce::AudioChannelSet set;
    for (int i = 0; i < channels; ++i) set.addChannel(kSpeakers[i]);
    return set;
}

remote::IRemote* remoteOf(juce::AudioPluginInstance& plugin, std::string_view kind, int version) {
    auto* client = plugin.getVST3Client();
    auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
    void* found = nullptr;
    if (component == nullptr || component->queryInterface(remote::kRemoteIid, &found) != Steinberg::kResultOk ||
        found == nullptr)
        return nullptr;
    auto* r = static_cast<remote::IRemote*>(found);
    const char* spoken = r->remoteKind();
    return spoken != nullptr && kind == spoken && r->remoteVersion() == version ? r : nullptr;
}

std::string remoteReport(remote::IRemote& remote) {
    std::string text(4096, '\0');
    for (;;) {
        const auto length = remote.remoteReport(text.data(), static_cast<Steinberg::int32>(text.size()));
        if (length < 0) return {};
        if (static_cast<std::size_t>(length) < text.size()) {
            text.resize(static_cast<std::size_t>(length));
            return text;
        }
        text.assign(static_cast<std::size_t>(length) + 1, '\0');
    }
}

const ParamManifest& manifestOf(live::Instance instance, live::Effect effect) {
    if (instance == live::Instance::Encoder) return encodeParams();
    if (instance == live::Instance::Decoder) return decodeParams();
    return effect == live::Effect::Reverb ? reverbParams() : echoParams();
}

juce::String describe(const juce::AudioProcessor::BusesLayout& l) {
    juce::StringArray parts;
    for (int i = 0; i < l.inputBuses.size(); ++i)
        parts.add((i == 0 ? "in " : "sidechain ") + juce::String(l.inputBuses[i].size()));
    for (int i = 0; i < l.outputBuses.size(); ++i) parts.add("out " + juce::String(l.outputBuses[i].size()));
    return parts.joinIntoString(ui::kSeparator);
}

Loaded loadInstalled(const juce::String& name, Role role, int order, double rate, int maxBlock, int outputs) {
    //  Every plugin this process loads joins one session, named once for the process before the first is made: a respawned
    //  host's plugins come back with states saved in the last run, and a DAW's session may be the machine's most recent.
    static const bool named = [] {
        return setenv(kHostSessionVariable, Uuid::generate().toString().c_str(), 1) == 0;
    }();
    (void)named;
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
    if (outs.size() > 0) outs.getReference(0) = role == Role::Decoder ? speakerChannels(outputs) : field;

    const bool took = out.plugin->setBusesLayout(layout);
    out.plugin->prepareToPlay(rate, maxBlock);
    out.status =
        out.plugin->getName() + ui::kSeparator + "VST3" + ui::kSeparator + describe(out.plugin->getBusesLayout());
    if (!took) {
        out.status += " -- refused " + describe(layout);
        out.plugin.reset();
    }
    return out;
}

}  // namespace bambi::livehost
