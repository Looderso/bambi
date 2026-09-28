// SPDX-License-Identifier: GPL-3.0-or-later
//  A PNG of the editor, so a UI change can be looked at without opening a host.
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/math/sh.hpp"
#include "bambi/ui/RegionClipboard.h"

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String out = argc > 1 ? argv[1] : "/tmp/reverb.png";

    BambiReverbProcessor proc;
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
    const bool regions = argc > 2 && juce::String(argv[2]) == "--regions";
    if (regions) {
        proc.document().edit("region kind",
                             [](bambi::PluginState& s) { s.regions[0].shape.kind = bambi::RegionKind::Spot; });
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* rev = dynamic_cast<BambiReverbEditor*>(editor.get());
    if (rev == nullptr) return 1;
    /*  The energy picture is off when a window opens, and while it is off the audio thread gathers
        nothing -- so the tool asks for it and lets a window of audio arrive, as a user who wanted
        to see it would. */
    rev->setEnergyShown(true);
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
        rev->tick();
    }
    rev->placeProbe(0.25, 0.15);
    rev->showTab(1);              // the regions tab
    rev->chooseRegionKind(0, 3);  // sectors, which has the most to show
    /*  --copy scrolls to the bottom of the regions tab, where copy and paste are, over a clipboard
        of the tool's own holding a band: paste lit, and its readout saying what it would bring. */
    const bool copyShot = argc > 2 && juce::String(argv[2]) == "--copy";
    if (copyShot) {
        bambi::ui::textClipboard() = {
            [] {
                return juce::String(
                    R"({"bambi.region":1,"kind":"band","fields":{"softness":140.4},"rows":[{"lfo":0,"field":"yaw","depth":0.5}]})");
            },
            [](const juce::String&) {}};
        bambi::ui::clipboardChanged();
    }
    /*  Audio keeps running while the editor ticks: the picture stops 1.6 s after the last
        covariance, so a shot that processed audio and then only ticked would draw an empty
        sphere. */
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
        rev->tick();
    }

    if (copyShot) rev->revealNamed("region copy");
    if (argc > 2 && juce::String(argv[2]) == "--settings") {
        rev->openSettings(true);
        rev->typeInstanceName("plate, long");
    }
    //  `--source N`: a source's own page, as a click on its matrix column opens it
    if (argc > 3 && juce::String(argv[2]) == "--source") {
        rev->showSource(juce::String(argv[3]).getIntValue());
        rev->tick();
    }
    const juce::Image shot = rev->createComponentSnapshot(rev->getLocalBounds(), true, 2.0f);
    juce::File file(out);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    png.writeImageToStream(shot, stream);
    std::printf("%s  %d x %d\n", out.toRawUTF8(), shot.getWidth(), shot.getHeight());
    return 0;
}
