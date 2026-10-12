// SPDX-License-Identifier: GPL-3.0-or-later
//  A PNG of the editor, so a UI change can be looked at without opening a host.
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/decode/layouts.hpp"
#include "bambi/host/TestPlayHead.h"
#include "bambi/math/sh.hpp"

/*  bambi-decoder-shot [out.png] [--speakers | --loudspeakers]
 *  The default shows the headphones page with the array's 7.1.4 layout loaded and a source moving; --speakers
 *  opens the speakers page, which draws the hull, and plays the layout; --loudspeakers opens its table too. */
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String out = argc > 1 ? argv[1] : "/tmp/decoder.png";
    const bool table = argc > 2 && juce::String(argv[2]) == "--loudspeakers";
    const bool speakers = table || (argc > 2 && juce::String(argv[2]) == "--speakers");
    BambiDecoderProcessor proc;
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::ambisonic(3));
    l.outputBuses.add(juce::AudioChannelSet::discreteChannels(18));
    proc.setBusesLayout(l);
    proc.prepareToPlay(48000.0, 128);
    bambi::host::TestPlayHead head;
    proc.setPlayHead(&head);
    proc.editDecoder("layout", [speakers](bambi::DecoderState& d) {
        d.layout = bambi::bs2051Layout("4+7+0")->speakers;
        d.layoutName = "4+7+0";
        d.speakersPlay = speakers;
        d.headphonesPlay = !speakers;
    });
    juce::MemoryBlock none;
    //  A field with a source in it, so the meters and the readouts have something to show.
    std::vector<double> y(16, 0.0);
    juce::AudioBuffer<float> buffer(18, 128);
    juce::MidiBuffer midi;
    for (int b = 0; b < 100; ++b) {
        bambi::shSN3D(bambi::fromAzEl(0.4 * std::sin(b * 0.03), 0.2), 3, y);
        head.time = b * 128;
        buffer.clear();
        for (int c = 0; c < 16; ++c) {
            auto* x = buffer.getWritePointer(c);
            for (int i = 0; i < 128; ++i)
                x[i] = static_cast<float>(y[static_cast<std::size_t>(c)] * 0.5 *
                                          std::sin(6.2831853 * 220.0 * (b * 128 + i) / 48000.0));
        }
        proc.processBlock(buffer, midi);
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* dec = dynamic_cast<BambiDecoderEditor*>(editor.get());
    if (dec == nullptr) return 1;
    if (speakers) dec->showOutput(1);
    if (table) dec->openTemp(bambi::ui::TempTab::Loudspeakers);
    dec->tick();
    dec->paintNow();
    juce::Image image(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
    juce::Graphics g(image);
    editor->paintEntireComponent(g, true);
    juce::File(out).deleteFile();
    juce::FileOutputStream stream{juce::File(out)};
    juce::PNGImageFormat png;
    if (!stream.openedOk() || !png.writeImageToStream(image, stream)) return 1;
    std::printf("wrote %s (%d x %d)\n", out.toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
