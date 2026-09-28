// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/Panels.h"

#include "bambi/ui/Draw.h"

namespace bambi::ui {
namespace colour = theme::colour;
namespace hdr = theme::header;
namespace mtr = theme::meter;

// ---- diagnostics -----------------------------------------------------------------------------

DiagnosticsView::DiagnosticsView(BambiEncoderProcessor& processor, FrameTrace& trace, std::function<void()> onRecord)
    : processor_(processor), trace_(trace), onRecord_(std::move(onRecord)) {
    setOpaque(true);
}

void DiagnosticsView::paint(juce::Graphics& g) {
    const auto& d = processor_.diagnostics();
    const auto mono = juce::Font(
        juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), theme::type::readoutSmall, juce::Font::plain));
    g.fillAll(colour::ground);

    const auto dB = [](float v) {
        return v > 0.0f ? juce::String(juce::Decibels::gainToDecibels(v), 1) + " dBFS" : juce::String("-inf");
    };
    auto format = juce::String(juce::AudioProcessor::getWrapperTypeDescription(processor_.wrapperType));
    if (processor_.wrapperType == juce::AudioProcessor::wrapperType_Undefined) format = "CLAP (or unknown)";
    const int order = d.order.load();
    const int lastNote = d.lastNote.load();
    const int inputChannels = d.mainInputChannels.load();
    const int outputChannels = d.outputChannels.load();
    const int unused = order >= 0 ? outputChannels - (order + 1) * (order + 1) : 0;
    const auto link = processor_.linkStatus();

    juce::StringArray lines;
    lines.add("diagnostics  --  the P2 host contract readout");
    lines.add("");
    lines.add("format        " + format);
    lines.add("sample rate   " + juce::String(d.sampleRate.load(), 0) + " Hz     block " +
              juce::String(d.blockSamples.load()) + " samples (max " + juce::String(d.blockSize.load()) + ")");
    lines.add("dsp load      " + juce::String(processor_.dspLoad(), 2) + " % of each block     peak " +
              juce::String(processor_.dspPeak(), 2) + " %");
    //  arriving / declared, for both buses: a host can say one and hand over the other.
    const auto widths = [](int arriving, int declared) {
        return arriving == declared ? juce::String(arriving) + " ch"
                                    : juce::String(arriving) + " ch arriving, " + juce::String(declared) + " declared";
    };
    lines.add("input         " + widths(inputChannels, d.mainBusChannels.load()) +
              (inputChannels > 2 ? juce::String(" (source: channels 1-2)") : juce::String()) + "     sidechain " +
              widths(d.sidechainChannels.load(), d.sidechainBusChannels.load()));
    lines.add("output        " + juce::String(outputChannels) + " ch  =  " +
              (order >= 0 ? "ambisonic order " + juce::String(order) +
                                (unused > 0 ? " + " + juce::String(unused) + " unused" : juce::String())
                          : juce::String("no output")));
    lines.add("transport     " + juce::String(d.playing.load() ? "playing" : "stopped") + "     offline render " +
              juce::String(d.nonRealtime.load() ? "yes" : "no") +
              "     t = " + juce::String(static_cast<juce::int64>(d.timeInSamples.load())));
    lines.add("lifecycle     prepares " + juce::String(d.prepares.load()) + "     resets " +
              juce::String(d.resets.load()) + "     layout mismatches " + juce::String(d.layoutMismatches.load()) +
              "     state v" + juce::String(static_cast<juce::int64>(d.stateSequence.load())));
    lines.add("MIDI          " + juce::String(d.midiEvents.load()) + " events     last note " +
              (lastNote >= 0 ? juce::String(lastNote) : juce::String("--")) + "     dropped " +
              juce::String(d.notesDropped.load()));
    lines.add("peaks         in " + dB(d.inputPeak.load()) + "     sidechain " + dB(d.sidechainPeak.load()) +
              "     out " + dB(d.outputPeak.load()));
    lines.add("source        speed " + juce::String(d.speed.load(), 1) + " deg/s     az " +
              juce::String(d.azimuthDeg.load(), 1) + "     el " + juce::String(d.elevationDeg.load(), 1));
    lines.add("link          " + (link.open ? "session " + link.session.substring(0, 8) + "   instance " +
                                                  link.instance.substring(0, 8) + "   slot " + juce::String(link.slot)
                                            : juce::String("not joined") +
                                                  (link.error.isNotEmpty() ? ": " + link.error : juce::String())));
    lines.add("              peers " + juce::String(link.peers) + "   live sessions " +
              juce::String(link.liveSessions) + "   rejoins " + juce::String(link.rejoins) + "   remote edits " +
              juce::String(link.remoteEdits));
    lines.add("timing trace  " +
              (trace_.status().isNotEmpty()
                   ? trace_.status()
                   : juce::String("click this line, then play: records 20 s of scene timing to /tmp")));
    const int traceLine = lines.size() - 1;
    lines.add("");
    lines.add("layout requests, newest first");
    lines.addArray(processor_.layoutLog());

    const auto lineHeight = mono.getHeight() * 1.6f;
    auto y = static_cast<float>(theme::space::inset);
    for (int i = 0; i < lines.size(); ++i) {
        const juce::Rectangle<float> box{static_cast<float>(theme::space::inset), y, static_cast<float>(getWidth()),
                                         lineHeight};
        text(g, lines[i], mono, i == traceLine ? colour::link : colour::text, box);
        if (i == traceLine) traceLine_ = box;
        y += lineHeight;
    }
}

void DiagnosticsView::mouseDown(const juce::MouseEvent& e) {
    if (traceLine_.contains(e.position) && !trace_.recording() && onRecord_) onRecord_();
}

}  // namespace bambi::ui
