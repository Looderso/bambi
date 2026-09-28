// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-conformance — the one suite for the suite.
//
//  Every plugin, asked the same questions three ways: its sources as each format's wrapper would make
//  them, its installed VST3 loaded through JUCE's VST3 hosting, and its installed CLAP loaded through the
//  CLAP entry point. The installed stages need the plugins installed (a plugin build does that).
//  Exit 0 means every check passed.

#include <cstdio>

#include "../../plugins/echo/Source/PluginProcessor.h"
#include "../../plugins/encoder/Source/PluginProcessor.h"
#include "../../plugins/reverb/Source/PluginProcessor.h"
#include "Conformance.h"

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("bambi-conformance\n");

    /*  A private session directory: these construct real processors, which join the link bus, and
        the machine's real directory may belong to a REAPER session running right now. */
    bambi::host::setLinkDirectoryNameForTests("/bambi.conf." + bambi::Uuid::generate().toString().substr(0, 8));

    using bambi::conformance::Subject;
    const Subject subjects[] = {
        {"bambi Encoder",
         [] { return std::unique_ptr<juce::AudioProcessor>(new BambiEncoderProcessor()); },
         false,
         {0, 1, 2, 3, 5, 7, 10}},
        {"bambi Echo",
         [] { return std::unique_ptr<juce::AudioProcessor>(new BambiEchoProcessor()); },
         true,
         {0, 1, 2, 3, 5, 7}},
        {"bambi Reverb",
         [] { return std::unique_ptr<juce::AudioProcessor>(new BambiReverbProcessor()); },
         true,
         {0, 1, 2, 3, 5, 7}},
    };

    bambi::conformance::Result total;
    std::printf("\nthe sources, as each format's wrapper would make them\n");
    for (const auto& subject : subjects)
        for (const auto format : {juce::AudioProcessor::wrapperType_VST3, juce::AudioProcessor::wrapperType_Undefined})
            total += bambi::conformance::run(subject, format);

    std::printf("\nthe installed plugins, loaded as a host loads them\n");
    for (const auto& subject : subjects) {
        total += bambi::conformance::probeInstalledVst3(subject);
        total += bambi::conformance::probeInstalledClap(subject);
    }

    std::printf("\n%d checks, %s\n", total.checks,
                total.failures == 0 ? "all pass" : (juce::String(total.failures) + " FAILED").toRawUTF8());
    return total.failures == 0 ? 0 : 1;
}
