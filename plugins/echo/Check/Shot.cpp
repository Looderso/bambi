// SPDX-License-Identifier: GPL-3.0-or-later
//  A PNG of the editor, so a UI change can be looked at without opening a host.
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/math/sh.hpp"
#include "bambi/region/projection.hpp"

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String out = argc > 1 ? argv[1] : "/tmp/echo.png";

    /*  --presets, --presets-name, --presets-delete: the browser over the panel, with a name being
        typed, and with the question asked. In a directory of the tool's own, set before the
        processor first looks for one, so nothing of the user's is read or written. */
    const bool presets = argc > 2 && juce::String(argv[2]).startsWith("--presets");
    const auto presetDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("bambi-shot-presets");
    if (presets) {
        presetDir.deleteRecursively();
        bambi::host::PluginProcessor::usePresetDirectory(presetDir.getFullPathName().toStdString());
    }

    BambiEchoProcessor proc;
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::ambisonic(3));
    l.inputBuses.add(juce::AudioChannelSet::stereo());
    l.outputBuses.add(juce::AudioChannelSet::ambisonic(3));
    proc.setBusesLayout(l);
    proc.prepareToPlay(48000.0, 128);

    bambi::host::TestPlayHead head;
    head.bpm = 95.0;
    proc.setPlayHead(&head);

    //  A field with real direction in it, so the picture has something to show.
    std::vector<double> y(16, 0.0);
    juce::AudioBuffer<float> buffer(18, 128);
    juce::MidiBuffer midi;
    for (int b = 0; b < 200; ++b) {
        const double az = 0.6 * std::sin(b * 0.02);
        bambi::shSN3D(bambi::fromAzEl(az, 0.3), 3, y);
        head.time = b * 128;
        buffer.clear();
        for (int c = 0; c < 16; ++c) {
            auto* x = buffer.getWritePointer(c);
            for (int i = 0; i < 128; ++i)
                x[i] = static_cast<float>(y[(size_t)c] * 0.5 * std::sin(6.2831853 * 180.0 * (b * 128 + i) / 48000.0));
        }
        proc.processBlock(buffer, midi);
    }

    /*  --regions opens the regions tab on a spot, so the shot shows the region in the scene with
        its handles and the energy at half under it. */
    const juce::String want = argc > 2 ? juce::String(argv[2]) : juce::String();
    const bool regions = want.startsWith("--region") || want == "--sectors" || want == "--dots" || want == "--full" ||
                         want == "--clouds" || want == "--custom";
    if (regions) {
        const auto kind = want == "--sectors"                      ? bambi::RegionKind::Sectors
                          : (want == "--dots" || want == "--full") ? bambi::RegionKind::Dots
                          : want == "--clouds"                     ? bambi::RegionKind::Clouds
                                                                   : bambi::RegionKind::Spot;
        //  --custom is a spot converted: the pyramid, with the spot's weights in it
        const bool custom = want == "--custom";
        proc.document().edit("region kind", [kind, custom](bambi::PluginState& s) {
            auto& r = s.regions[0].shape;
            r.kind = kind;
            if (custom) {
                bambi::Region spot;
                spot.kind = bambi::RegionKind::Spot;
                spot.size = 60.0 * bambi::kDeg2Rad;
                spot.softness = 30.0 * bambi::kDeg2Rad;
                bambi::projectWeights(spot, bambi::kRegionWeightsOrder, r.weights);
                r.custom = true;
            }
        });
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* echo = dynamic_cast<BambiEchoEditor*>(editor.get());
    if (echo == nullptr) return 1;
    /*  The energy picture is off when a window opens, and while it is off the audio thread
        gathers nothing -- so the tool asks for it and lets a window of audio arrive, as a user who
        wanted to see it would. */
    echo->setEnergyShown(true);
    for (int b = 200; b < 260; ++b) {
        const double az = 0.6 * std::sin(b * 0.02);
        bambi::shSN3D(bambi::fromAzEl(az, 0.3), 3, y);
        head.time = b * 128;
        buffer.clear();
        for (int c = 0; c < 16; ++c) {
            auto* x = buffer.getWritePointer(c);
            for (int i = 0; i < 128; ++i)
                x[i] = static_cast<float>(y[(size_t)c] * 0.5 * std::sin(6.2831853 * 180.0 * (b * 128 + i) / 48000.0));
        }
        proc.processBlock(buffer, midi);
        echo->tick();
    }
    //  turn on a second tap, so the strip has more than one row to show
    if (auto* p = proc.hostParameter(static_cast<bambi::ParamId>(bambi::echoParams().byKey("tap2.on"))))
        p->setValueNotifyingHost(1.0f);
    if (want == "--lfo") echo->openSource(12);
    if (want == "--env") echo->openSource(15);
    /*  --env-held: a held gate and full polarity, which is the only way to see the plateau, the
        shading that says it is a preview, and the axis polarity relabels. */
    if (want == "--env-held") {
        proc.document().edit("held", [](bambi::PluginState& s) { s.envTriggers[0].gate = bambi::TriggerGate::Held; });
        if (auto* p = proc.hostParameter(static_cast<bambi::ParamId>(bambi::echoParams().byKey("env1.polarity"))))
            p->setValueNotifyingHost(1.0f);
        echo->openSource(15);
    }
    if (want == "--full") echo->setEquirectFull(true);
    if (regions)
        echo->showTab(1);
    else
        echo->placeProbe(0.25, 0.15);
    /*  Audio keeps running while the editor ticks. The picture stops 1.6 s after the last covariance,
        so a shot that processed audio and then only ticked would draw an empty sphere. */
    for (int frame = 0; frame < 60; ++frame) {
        for (int b = 0; b < 13; ++b)  // about a thirtieth of a second of audio a frame
        {
            const double az = 0.6 * std::sin((200 + frame * 13 + b) * 0.02);
            bambi::shSN3D(bambi::fromAzEl(az, 0.3), 3, y);
            head.time = (200 + frame * 13 + b) * 128;
            buffer.clear();
            for (int c = 0; c < 16; ++c) {
                auto* x = buffer.getWritePointer(c);
                for (int i = 0; i < 128; ++i)
                    x[i] = static_cast<float>(y[(size_t)c] * 0.5 *
                                              std::sin(6.2831853 * 180.0 * (head.time + i) / 48000.0));
            }
            proc.processBlock(buffer, midi);
        }
        echo->tick();
    }

    if (presets) {
        for (const char* path :
             {"verse delay", "songs/bridge, long", "songs/outro wash", "drums/slap", "drums/tight room"})
            proc.savePreset(*bambi::parsePresetPath(path));
        proc.loadPreset({false, "songs", "outro wash"});
        if (auto* p = proc.hostParameter(static_cast<bambi::ParamId>(bambi::echoParams().byKey("tap1.blur"))))
            p->setValueNotifyingHost(0.8f);
        proc.presets().setFolded("user/drums", true);
        echo->openPresets(true);
        echo->tick();
        echo->paintNow();
        if (want == "--presets-name") echo->clickPresetAction(bambi::ui::PresetBrowser::Action::SaveAs);
        if (want == "--presets-delete") echo->clickPresetAction(bambi::ui::PresetBrowser::Action::Delete);
        echo->tick();
    }

    const juce::Image shot = echo->createComponentSnapshot(echo->getLocalBounds(), true, 2.0f);
    juce::File file(out);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    png.writeImageToStream(shot, stream);
    std::printf("%s  %d x %d\n", out.toRawUTF8(), shot.getWidth(), shot.getHeight());
    if (presets) presetDir.deleteRecursively();
    return 0;
}
