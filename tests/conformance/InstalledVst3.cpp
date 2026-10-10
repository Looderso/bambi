// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "Conformance.h"

namespace bambi::conformance {
namespace {

juce::String describe(const juce::AudioProcessor::BusesLayout& l) {
    const auto set = [](const juce::AudioChannelSet& s) {
        return s.isDisabled() ? juce::String("off") : juce::String(s.size()) + " ch";
    };
    juce::String s = "in " + set(l.getMainInputChannelSet());
    s += "  |  sc " + (l.inputBuses.size() > 1 ? set(l.inputBuses[1]) : juce::String("none"));
    return s + "  |  out " + set(l.getMainOutputChannelSet());
}

void probeInstance(Result& r, juce::AudioPluginInstance& p, const juce::String& formatName, Kind kind) {
    const auto check = [&r](bool ok, const juce::String& what) { report(r, ok, what); };

    std::printf("    as loaded:   %s\n", describe(p.getBusesLayout()).toRawUTF8());
    if (kind == Kind::Decoder)
        check(p.getBusCount(true) == 1, "it presents one input bus, the field, and no sidechain");
    else
        check(p.getBusCount(true) > 1, "it presents a sidechain bus at all");
    if (kind != Kind::Decoder && p.getBusCount(true) > 1) {
        auto* sc = p.getBus(true, 1);
        check(sc->isEnabled() && sc->getNumberOfChannels() > 0,
              "and it arrives enabled, " + juce::String(sc->getNumberOfChannels()) + " ch");
        //  What a host does when the user routes a sidechain: ask for it explicitly.
        check(sc->setCurrentLayout(juce::AudioChannelSet::stereo()), "a host can ask for a stereo sidechain");
    }

    //  A track wider than stereo, asked for as REAPER asks: as many inputs as outputs, through the real wrapper.
    for (const int order : {1, 3, 5}) {
        const int channels = (order + 1) * (order + 1);
        //  Ambisonic, not discrete: JUCE's VST3 host cannot name a discrete set as a speaker arrangement.
        const auto set = juce::AudioChannelSet::ambisonic(order);
        auto wide = p.getBusesLayout();
        wide.getChannelSet(true, 0) = set;
        wide.getChannelSet(false, 0) = set;
        const bool took = p.setBusesLayout(wide);
        const auto now = p.getBusesLayout();
        std::printf("    %d-channel track: %s, now %s\n", channels, took ? "accepted" : "refused",
                    describe(now).toRawUTF8());
        if (kind == Kind::Decoder) {
            /*  The field arrives on the main bus and the output is whatever the track is: a wide track is taken
                whole too. Then, asked for a stereo output on the same field, the input must not move with it. */
            check(took && now.getMainInputChannelSet().size() == channels &&
                      now.getMainOutputChannelSet().size() == channels,
                  "a " + juce::String(channels) + "-channel track is taken whole, the field in and the bus out");
            auto pair = p.getBusesLayout();
            pair.getChannelSet(false, 0) = juce::AudioChannelSet::stereo();
            const bool tookPair = p.setBusesLayout(pair);
            check(tookPair && p.getBusesLayout().getMainInputChannelSet().size() == channels &&
                      p.getBusesLayout().getMainOutputChannelSet().size() == 2,
                  "and a stereo output on the same field leaves the field at " + juce::String(channels));
        } else if (kind == Kind::FieldEffect) {
            //  The field arrives on the main bus; an effect must never take a different width in from out.
            check(took && now.getMainInputChannelSet().size() == channels &&
                      now.getMainOutputChannelSet().size() == channels,
                  "a " + juce::String(channels) + "-channel track is taken whole, in and out");
            auto mismatched = p.getBusesLayout();
            mismatched.getChannelSet(true, 0) = juce::AudioChannelSet::ambisonic(1);
            mismatched.getChannelSet(false, 0) = set;
            p.setBusesLayout(mismatched);
            check(p.getBusesLayout().getMainInputChannelSet().size() ==
                      p.getBusesLayout().getMainOutputChannelSet().size(),
                  "and it never settles on a layout whose in and out differ");
            p.setBusesLayout(wide);
        } else {
            //  The encoder keeps its main input stereo, so track channels 3/4 are the sidechain.
            if (formatName == "VST3")
                check(now.getMainInputChannelSet().size() <= 2,
                      "as VST3 the main input stays stereo on a " + juce::String(channels) + "-channel track");
            auto outOnly = p.getBusesLayout();
            outOnly.getChannelSet(true, 0) = juce::AudioChannelSet::stereo();
            outOnly.getChannelSet(false, 0) = set;
            const bool tookOut = p.setBusesLayout(outOnly);
            check(tookOut && p.getBusesLayout().getMainOutputChannelSet().size() == channels,
                  "asked with a stereo input, the output follows a " + juce::String(channels) + "-channel track");
        }
    }
}

/// Where an installed VST3 is, in each system's standard folder.
juce::File installedVst3(const std::string& name) {
    const auto bundle = juce::String(name) + ".vst3";
#if JUCE_WINDOWS
    return juce::File(
               juce::SystemStats::getEnvironmentVariable("CommonProgramFiles", "C:\\Program Files\\Common Files"))
        .getChildFile("VST3")
        .getChildFile(bundle);
#else
    const juce::File home(juce::File::getSpecialLocation(juce::File::userHomeDirectory));
#if JUCE_MAC
    return home.getChildFile("Library/Audio/Plug-Ins/VST3").getChildFile(bundle);
#else
    return home.getChildFile(".vst3").getChildFile(bundle);
#endif
#endif
}

}  // namespace

Result probeInstalledVst3(const Subject& subject) {
    Result r;
    std::printf("\n%s, the installed VST3\n", subject.name.c_str());
    juce::VST3PluginFormat format;
    //  Only our own bundle: scanning the default locations would load every plugin on the machine.
    const auto file = installedVst3(subject.name);
    juce::OwnedArray<juce::PluginDescription> descriptions;
    if (file.exists() && format.fileMightContainThisPluginType(file.getFullPathName()))
        format.findAllTypesForFile(descriptions, file.getFullPathName());
    if (descriptions.isEmpty()) {
        report(r, false, "not installed, so never loaded as a host loads it: " + file.getFullPathName());
        return r;
    }
    for (const auto* description : descriptions) {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> p(
            format.createInstanceFromDescription(*description, 48000.0, 512, error));
        if (p == nullptr) {
            report(r, false, "could not instantiate: " + error);
            continue;
        }
        probeInstance(r, *p, description->pluginFormatName, subject.kind);
    }
    return r;
}

}  // namespace bambi::conformance
