// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <vector>

/*  One suite for the suite: every plugin must negotiate its ambisonic layout and accept a
 *  sidechain, in both formats, in a real host. Until that is boring, no amount of shared UI is
 *  worth building on top.
 *
 *  Six checks, every plugin, both formats.
 *
 *  What differs between plugins is only what they claim -- the encoder takes a mono or stereo
 *  source and makes a field; a field effect takes the field and returns it at the same width.
 *  That difference is a `Subject`, and everything else here is the same question asked of each.
 */
namespace bambi::conformance {

/// One plugin under test: how to make one, and what it says it accepts.
struct Subject {
    std::string name;
    /// A fresh processor. The caller has already set the wrapper type this round stands for.
    std::function<std::unique_ptr<juce::AudioProcessor>()> make;

    /*  A field effect takes the field in and returns it at the same width; the encoder takes a
     *  source. This is the whole of what `isBusesLayoutSupported` differs by across the suite,
     *  so it is the whole of what a Subject has to say. */
    bool fieldEffect{true};

    /// The orders it is expected to accept on its main output.
    std::vector<int> orders{0, 1, 2, 3, 5, 7, 10};
};

struct Result {
    int checks{0};
    int failures{0};

    Result& operator+=(const Result& other) {
        checks += other.checks;
        failures += other.failures;
        return *this;
    }
};

/// Prints one check, "ok" or "FAIL", and counts it.
inline void report(Result& r, bool ok, const juce::String& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    ++r.checks;
    if (!ok) ++r.failures;
}

/*  Run the six against `subject`, as `format` (VST3 or CLAP -- which decides only what
 *  `setTypeOfNextNewPlugin` reports, since the rule is not format-conditional and this suite
 *  exists partly to keep it that way). Prints as it goes.
 */
Result run(const Subject& subject, juce::AudioProcessor::WrapperType format);

/*  The installed VST3 (~/Library/Audio/Plug-Ins/VST3/<name>.vst3), loaded through JUCE's VST3 hosting as a
    host loads it: its buses as loaded, a sidechain a host can ask for, and wide tracks. */
Result probeInstalledVst3(const Subject& subject);

/*  The installed CLAP (~/Library/Audio/Plug-Ins/CLAP/<name>.clap), loaded through the CLAP entry point with no
    JUCE in between: its audio ports, the ambisonic extension, and every port configuration. */
Result probeInstalledClap(const Subject& subject);

}  // namespace bambi::conformance
