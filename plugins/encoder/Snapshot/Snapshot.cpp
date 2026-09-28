// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-plugin-snapshot -- the editor, rendered to a PNG, with a small session in it.
//
//  Looking is the only way to check the UI. This renders the real editor over real processors --
//  five instances on one link bus, each with its own shape, speed and placement, run for a few
//  seconds so their sources have moved -- with no host, so a UI change is checked in seconds
//  rather than by loading REAPER.
//
//    bambi-plugin-snapshot out.png [--scale 2] [--select N] [--tab 0-2] [--matrix-tab 0-2] [--provisional KEY] [--energy]
//                           [--tracks N]  -- more instances than the fixture, to see the header's tabs overflow
//                           [--full] [--source 0-18] [--show N] [--nodes] [--width DEG] [--region-kind 0-4]
//                           [--complex]  -- every shape at its most intricate settings
//                           [--frames N] -- paint the editor N more times and report the time a frame takes

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/encode/params.hpp"
#include "bambi/host/TestPlayHead.h"
#include "bambi/link/directory.hpp"
#include "bambi/link/link.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/authoring.hpp"
#include "bambi/path/generator.hpp"

namespace {
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

using PlayingHead = bambi::host::TestPlayHead;

struct Track {
    std::string name;
    bambi::GeneratorType shape;
    float speed;     ///< deg/s
    float yaw;       ///< deg
    float pitch;     ///< deg
    float yawRate;   ///< deg/s: the placement turning under its own rate
    double seconds;  ///< how long it runs before the snapshot
};
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;

    std::string out = "bambi-editor.png";
    float scale = 1.0f;
    int select = 1;
    int tab = 0, matrixTab = 0;
    std::string provisional;
    int source = -1;
    int show = -1;          // another instance, selected in the editor's scene
    bool nodes = false;     // the selected instance's path converted to a custom chain, with a node selected
    float width = -1.0f;    // --width DEG: every instance's width, to see the selected one's drawn
    bool energy = false;    // --energy: the energy picture on, which it is not when a window opens
    int inputMode = -1;     // --input-mode 0-2: every instance's stereo input mode, to see its two points drawn
    bool complex = false;   // --complex: the paths that are hardest to draw smoothly
    int frames = 0;         // --frames N: time N more paints of the whole editor
    int trackCount = 0;     // --tracks N: pad the fixture out, to see the header's tabs overflow and scroll
    int regionKind = -1;    // --region-kind N: the region's shape, so its source page has one to draw
    bool full = false;      // --full: the equirect takes the whole scene row, the globe hidden
    bool settings = false;  // --settings: the settings page open, over a named instance
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scale" && i + 1 < argc)
            scale = std::stof(argv[++i]);
        else if (a == "--select" && i + 1 < argc)
            select = std::stoi(argv[++i]);
        else if (a == "--tab" && i + 1 < argc)
            tab = std::stoi(argv[++i]);
        else if (a == "--matrix-tab" && i + 1 < argc)
            matrixTab = std::stoi(argv[++i]);
        else if (a == "--provisional" && i + 1 < argc)
            provisional = argv[++i];
        else if (a == "--source" && i + 1 < argc)
            source = std::stoi(argv[++i]);
        else if (a == "--show" && i + 1 < argc)
            show = std::stoi(argv[++i]);
        else if (a == "--tracks" && i + 1 < argc)
            trackCount = std::stoi(argv[++i]);
        else if (a == "--settings")
            settings = true;
        else if (a == "--full")
            full = true;
        else if (a == "--nodes")
            nodes = true;
        else if (a == "--energy")
            energy = true;
        else if (a == "--input-mode" && i + 1 < argc)
            inputMode = std::stoi(argv[++i]);
        else if (a == "--width" && i + 1 < argc)
            width = std::stof(argv[++i]);
        else if (a == "--region-kind" && i + 1 < argc)
            regionKind = std::stoi(argv[++i]);
        else if (a == "--complex")
            complex = true;
        else if (a == "--frames" && i + 1 < argc)
            frames = std::stoi(argv[++i]);
        else
            out = a;
    }

    //  A private session directory: never the machine's, which a running REAPER may be using.
    const std::string directory = "/bambi.ps." + bambi::Uuid::generate().toString().substr(0, 8);
    BambiEncoderProcessor::setLinkDirectoryNameForTests(directory);

    //  Two of them turn under their own rate, so a session shows both kinds of placement and the live
    //  mark has something to mark.
    const std::vector<Track> fixture{
        {"kick", bambi::GeneratorType::Orbit, 40.0f, -40.0f, 10.0f, 0.0f, 2.2},
        {"rhodes", bambi::GeneratorType::Lissajous, 90.0f, 0.0f, 0.0f, 25.0f, 3.1},
        {"pad wide", bambi::GeneratorType::Wave, 20.0f, 140.0f, -10.0f, 0.0f, 1.7},
        {"vox lead", bambi::GeneratorType::Arc, 60.0f, 10.0f, 30.0f, -12.0f, 2.6},
        {"shaker", bambi::GeneratorType::Spiral, 120.0f, -120.0f, 0.0f, 0.0f, 1.3},
    };

    //  --tracks pads the session out with more of the same shapes, under their own names: enough to push
    //  the header's tabs past the width they have and make the scrolling worth looking at.
    std::vector<Track> tracks = fixture;
    const int want = std::min(trackCount, bambi::kLinkMaxInstances);
    for (int i = static_cast<int>(tracks.size()); i < want; ++i) {
        auto t = fixture[static_cast<std::size_t>(i) % fixture.size()];
        t.name = "track " + std::to_string(i + 1);
        tracks.push_back(t);
    }

    std::vector<std::unique_ptr<BambiEncoderProcessor>> processors;
    std::vector<std::unique_ptr<PlayingHead>> heads;
    for (const auto& track : tracks) {
        auto proc = std::make_unique<BambiEncoderProcessor>();
        bambi::PluginState state{bambi::encodeParams()};
        state.trajectory.generator = track.shape;
        bambi::generatorDefaults(track.shape, state.trajectory.genParams);
        if (complex) {
            //  Each shape at the top of its ranges: the longest, most folded paths there are to draw.
            auto& p = state.trajectory.genParams;
            if (track.shape == bambi::GeneratorType::Lissajous)
                p[0] = 180.0, p[1] = 90.0, p[2] = 7.0, p[3] = 7.0;  // az amount, el amount, az ratio, el ratio
            else if (track.shape == bambi::GeneratorType::Wave)
                p[0] = 4.0, p[2] = 9.0;  // turns, wobbles
            else if (track.shape == bambi::GeneratorType::Spiral)
                p[0] = 6.0;  // turns
        }
        if (track.name == "rhodes")  // the matrix the selected instance shows
            state.matrix = {{bambi::MatrixTab::Features, 0, bambi::EncoderParam::MotionSpeed, 0.5},
                            {bambi::MatrixTab::Features, 2, bambi::EncoderParam::RenderWidth, 0.6},
                            {bambi::MatrixTab::Features, 1, bambi::EncoderParam::MotionDisplace, 0.35},
                            {bambi::MatrixTab::Generators, 0, bambi::EncoderParam::TransformYaw, 0.45}};
        if (track.name == "rhodes")  // envelope 2 fires from audio, so both kinds of trigger render
            state.envTriggers[1].input = bambi::TriggerInput::Audio;
        if (nodes &&
            static_cast<int>(processors.size()) == std::clamp(select, 0, static_cast<int>(tracks.size()) - 1)) {
            //  A chain to edit: the shape converted once, as the trajectory tab's "custom" does.
            bambi::convertToCustom(state.trajectory, 8);
            state.trajectory.kind = bambi::TrajectoryKind::Custom;
        }
        const std::string text = bambi::saveState(bambi::Product::Encoder, bambi::encodeParams(), state);
        proc->setStateInformation(text.data(), static_cast<int>(text.size()));

        juce::AudioProcessor::TrackProperties props;
        props.name = juce::String(track.name);
        proc->updateTrackProperties(props);

        const auto set = [&](bambi::ParamId id, float value) {
            if (auto* param = proc->hostParameter(id)) param->setValueNotifyingHost(param->convertTo0to1(value));
        };
        set(bambi::EncoderParam::MotionSpeed, track.speed);
        set(bambi::EncoderParam::TransformYaw, track.yaw);
        set(bambi::EncoderParam::TransformPitch, track.pitch);
        set(bambi::EncoderParam::TransformYawRate, track.yawRate);
        if (width >= 0.0f) set(bambi::EncoderParam::RenderWidth, width);
        if (inputMode >= 0) {
            set(bambi::EncoderParam::InputMode, static_cast<float>(inputMode));
            set(bambi::EncoderParam::InputOffset, 0.12f);
        }

        auto head = std::make_unique<PlayingHead>();
        proc->setPlayHead(head.get());
        proc->setRateAndBufferSizeDetails(kRate, kBlock);
        proc->prepareToPlay(kRate, kBlock);
        processors.push_back(std::move(proc));
        heads.push_back(std::move(head));
    }

    //  Join the session: the first join waits a few ticks, by design.
    for (int i = 0; i < BambiEncoderProcessor::kLinkTickHz / 10 + 2; ++i)
        for (auto& proc : processors) proc->linkTick();

    //  Run each for its own while, so the sources are spread along their paths.
    juce::MidiBuffer midi;
    for (std::size_t k = 0; k < processors.size(); ++k) {
        auto& proc = *processors[k];
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        kBlock);
        const int blocks = static_cast<int>(tracks[k].seconds * kRate / kBlock);
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            for (int i = 0; i < kBlock; ++i)
                buffer.setSample(
                    0, i, 0.25f * std::sin(0.02f * static_cast<float>((b * kBlock + i) * static_cast<int>(k + 1))));
            heads[k]->time = static_cast<juce::int64>(b) * kBlock;
            midi.clear();
            if (b == blocks - 3)  // a hit just before the snapshot, so envelope 1 has something to show
                midi.addEvent(juce::MidiMessage::noteOn(10, 36, 1.0f), 0);
            proc.processBlock(buffer, midi);
        }
    }
    for (int i = 0; i < 2; ++i)
        for (auto& proc : processors) proc->linkTick();

    auto& subject =
        *processors[static_cast<std::size_t>(std::clamp(select, 0, static_cast<int>(processors.size()) - 1))];
    int result = 0;
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(subject.createEditor());
        if (auto* shell = dynamic_cast<BambiEncoderEditor*>(editor.get())) {
            shell->showControls(tab, static_cast<bambi::MatrixTab>(std::clamp(matrixTab, 0, 3)),
                                provisional.empty() ? bambi::kNoParamId : bambi::parameterByKey(provisional));
            if (show >= 0 && show < static_cast<int>(processors.size()))
                shell->selectInstance(processors[static_cast<std::size_t>(show)]->identity().instance);
            if (regionKind >= 0) shell->chooseRegionKind(regionKind);
            if (full) shell->setEquirectFull(true);
            if (source >= 0) shell->showSource(source);
            if (nodes) shell->showNode(2);
            if (energy) {
                /*  Asked for, and then a window of audio let arrive: while the picture is off the
                    audio thread gathers nothing for it. More of the same signal, from where
                    the run above stopped, so the source has not jumped. */
                shell->setEnergyShown(true);
                const auto k = static_cast<std::size_t>(std::clamp(select, 0, static_cast<int>(processors.size()) - 1));
                juce::AudioBuffer<float> buffer(
                    std::max(subject.getTotalNumInputChannels(), subject.getTotalNumOutputChannels()), kBlock);
                const int from = static_cast<int>(tracks[k].seconds * kRate / kBlock);
                for (int b = from; b < from + 40; ++b) {
                    buffer.clear();
                    for (int i = 0; i < kBlock; ++i)
                        buffer.setSample(
                            0, i,
                            0.25f * std::sin(0.02f * static_cast<float>((b * kBlock + i) * static_cast<int>(k + 1))));
                    heads[k]->time = static_cast<juce::int64>(b) * kBlock;
                    midi.clear();
                    subject.processBlock(buffer, midi);
                    shell->tick();
                }
            }
        }
        if (frames > 0) {
            //  Every view painted whole, as a frame that repaints everything would: the cost of drawing the paths.
            const auto start = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < frames; ++i) editor->createComponentSnapshot(editor->getLocalBounds(), true, scale);
            std::printf("painted the editor %d times: %.2f ms a frame\n", frames,
                        (juce::Time::getMillisecondCounterHiRes() - start) / frames);
        }
        if (settings)
            if (auto* encoder = dynamic_cast<BambiEncoderEditor*>(editor.get())) encoder->openSettings(true);
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, scale);
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(out));
        file.deleteFile();
        juce::FileOutputStream stream(file);
        if (stream.openedOk() && juce::PNGImageFormat().writeImageToStream(image, stream)) {
            std::printf("wrote %s (%d x %d), %zu instances in the session\n", file.getFullPathName().toRawUTF8(),
                        image.getWidth(), image.getHeight(), processors.size());
        } else {
            std::fprintf(stderr, "could not write %s\n", out.c_str());
            result = 1;
        }
    }

    const auto session = processors.front()->identity().session;
    processors.clear();
    if (!session.isNil()) bambi::LinkBus::unlinkSession(session);
    bambi::LinkDirectory::unlink(directory);
    return result;
}
