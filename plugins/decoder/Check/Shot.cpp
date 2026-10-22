// SPDX-License-Identifier: GPL-3.0-or-later
//  A PNG of the editor, so a UI change can be looked at without opening a host.
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/decode/layouts.hpp"
#include "bambi/host/LinkNode.h"
#include "bambi/host/ShotScale.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/link/link.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/sphere.hpp"

/*  bambi-decoder-shot [out.png] [--speakers | --decoding | --fill | --loudspeakers | --energy | --sources |
 *                     --tracking]
 *                     [--scale N]
 *  The default shows the headphones page with the array's 7.1.4 layout loaded and a source moving; --speakers
 *  opens the speakers page, which draws the hull, and plays the layout; --loudspeakers opens its table too;
 *  --energy switches the energy picture on; --sources puts six Encoders on the bus and opens the sources list;
 *  --decoding opens its decoding sub-tab; --fill (or --gaps) its fill sub-tab with the fill on; --tracking reads OSC and opens its setup. */
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String out = argc > 1 ? argv[1] : "/tmp/decoder.png";
    const float scale = bambi::host::takeShotScale(argc, argv);
    const bool table = argc > 2 && juce::String(argv[2]) == "--loudspeakers";
    const bool energy = argc > 2 && juce::String(argv[2]) == "--energy";
    const bool sources = argc > 2 && juce::String(argv[2]) == "--sources";
    const bool gaps = argc > 2 && (juce::String(argv[2]) == "--fill" || juce::String(argv[2]) == "--gaps");
    const bool decoding = argc > 2 && juce::String(argv[2]) == "--decoding";
    const bool tracking = argc > 2 && juce::String(argv[2]) == "--tracking";
    const bool speakers = table || gaps || decoding || (argc > 2 && juce::String(argv[2]) == "--speakers");
    BambiDecoderProcessor proc;
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::ambisonic(3));
    l.outputBuses.add(juce::AudioChannelSet::discreteChannels(18));
    proc.setBusesLayout(l);
    proc.prepareToPlay(48000.0, 128);
    bambi::host::TestPlayHead head;
    proc.setPlayHead(&head);
    proc.editDecoder("layout", [speakers, gaps, tracking](bambi::DecoderState& d) {
        d.layout = bambi::bs2051Layout("4+7+0")->speakers;
        d.layoutName = "4+7+0";
        d.speakersPlay = speakers;
        d.headphonesPlay = !speakers;
        d.fill = gaps;
        if (tracking) d.trackerSource = 1;
    });
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* dec = dynamic_cast<BambiDecoderEditor*>(editor.get());
    if (dec == nullptr) return 1;
    if (energy) {
        dec->showEnergy();
        dec->tick();  // the window asks for the picture before the field runs
    }
    //  six Encoders of another window, on the Decoder's own bus: the sources its scene draws and its list names
    std::vector<std::unique_ptr<bambi::LinkBus>> others;
    if (sources) {
        for (int i = 0; i < bambi::host::LinkNode::kTickHz / 10 + 4; ++i) proc.linkTick();
        const char* names[] = {"kick", "bass", "pad", "vox", "room", "lead"};
        for (int i = 0; i < 6; ++i) {
            auto bus = std::make_unique<bambi::LinkBus>();
            if (!bus->open(proc.identity().session, bambi::Uuid::generate(), bambi::Product::Encoder)) return 1;
            bambi::LinkStatic st;
            st.setLabel(names[i]);
            bus->publishStatic(st);
            bambi::LinkDynamic d;
            const bambi::Vec3 at = bambi::fromAzEl((i * 60.0 - 100.0) * bambi::kDeg2Rad, (i % 3 - 1) * 0.35);
            d.x = static_cast<float>(at.x);
            d.y = static_cast<float>(at.y);
            d.z = static_cast<float>(at.z);
            bus->publishDynamic(d);
            others.push_back(std::move(bus));
        }
        for (int i = 0; i < 4; ++i) {
            proc.linkTick();
            dec->tick();
        }
    }
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
    if (speakers) dec->showOutput(1);
    if (gaps) dec->showFill();
    if (decoding) dec->showDecoding();
    if (sources) {
        dec->tick();
        dec->clickSourcesTriangle();  // the list opens
        dec->clickSourcesRow(1);      // the second source off
        dec->tick();
        dec->paintNow();
    }
    if (table) dec->openTemp(bambi::ui::TempTab::Loudspeakers);
    if (tracking) dec->openTemp(bambi::ui::TempTab::Tracking);
    dec->tick();
    dec->paintNow();
    juce::Image image(juce::Image::ARGB, juce::roundToInt(editor->getWidth() * scale),
                      juce::roundToInt(editor->getHeight() * scale), true);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));
    editor->paintEntireComponent(g, true);
    juce::File(out).deleteFile();
    juce::FileOutputStream stream{juce::File(out)};
    juce::PNGImageFormat png;
    if (!stream.openedOk() || !png.writeImageToStream(image, stream)) return 1;
    std::printf("wrote %s (%d x %d)\n", out.toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
