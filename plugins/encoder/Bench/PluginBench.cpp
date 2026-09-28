// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-plugin-bench — the plugin's own code, measured without a host.
//
//  A DAW's CPU meter answers "how busy is the DAW", not "what does this plugin cost", and it
//  cannot separate the audio thread from a window redrawing on the message thread. This runs the
//  exact processor and editor the plugin ships — same sources, same flags — and times each:
//  processBlock against a playing transport, and a full paint of the editor.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <random>
#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/ui/ProbeView.h"
#include "bambi/ui/RegionOverlay.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kRate = 48000.0;

using PlayingHead = bambi::host::TestPlayHead;

void setParameter(juce::AudioProcessor& proc, const juce::String& id, float value) {
    for (auto* param : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            if (ranged->paramID == id) ranged->setValueNotifyingHost(ranged->convertTo0to1(value));
}

struct Stats {
    double median = 0, p99 = 0, max = 0, coreShare = 0;
};

bool runProcessor(int order, int block, Stats& out, bool energyLooked = false) {
    BambiEncoderProcessor proc;
    proc.wantEnergy(energyLooked);  // the audio thread gathers for the energy picture only while a window has it on
    auto layout = proc.getBusesLayout();
    const int channels = (order + 1) * (order + 1);
    layout.getChannelSet(false, 0) =
        order <= 7 ? juce::AudioChannelSet::ambisonic(order) : juce::AudioChannelSet::discreteChannels(channels);
    if (!proc.setBusesLayout(layout)) return false;

    PlayingHead head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(kRate, block);
    setParameter(proc, "motion.speed", 90.0f);
    setParameter(proc, "render.width", 30.0f);
    proc.prepareToPlay(kRate, block);

    const int bufferChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(bufferChannels, block);
    juce::MidiBuffer midi;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);

    const int blocks = static_cast<int>(10.0 * kRate / block);  // ten seconds of audio
    constexpr int warmup = 50;
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(blocks));
    double total = 0.0;
    for (int b = 0; b < blocks + warmup; ++b) {
        buffer.clear();
        for (int c = 0; c < std::min(4, bufferChannels); ++c)  // main input and sidechain
            for (int i = 0; i < block; ++i) buffer.setSample(c, i, noise(rng));

        const auto t0 = Clock::now();
        proc.processBlock(buffer, midi);
        const double t = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        head.time += block;
        if (b >= warmup) {
            us.push_back(t);
            total += t;
        }
    }
    proc.releaseResources();

    std::sort(us.begin(), us.end());
    const double audioUs = static_cast<double>(blocks) * block / kRate * 1e6;
    out.median = us[us.size() / 2];
    out.p99 = us[us.size() * 99 / 100];
    out.max = us.back();
    out.coreShare = total / audioUs * 100.0;
    return true;
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
#if JUCE_DEBUG
    std::printf("\n  *** DEBUG BUILD -- these numbers are meaningless. ***\n");
#endif

    std::printf("\nbambi-plugin-bench   (48 kHz, transport playing, speed 90, width 30, sidechain fed)\n\n");
    //  with nobody looking at the energy picture, and with somebody
    for (const bool looked : {false, true}) {
        std::printf(looked ? "\nprocessBlock, the energy picture ON in a window\n" : "processBlock\n");
        std::printf("  order    ch  block      median         p99         max    one core   3 instances\n");
        for (int order : {1, 3, 5, 7, 10}) {
            for (int block : {128, 512}) {
                Stats s;
                if (!runProcessor(order, block, s, looked)) {
                    std::printf("  %5d  layout refused by the processor\n", order);
                    continue;
                }
                std::printf("  %5d  %4d  %5d  %8.3f us  %8.3f us  %8.3f us  %8.3f%%  %10.3f%%\n", order,
                            (order + 1) * (order + 1), block, s.median, s.p99, s.max, s.coreShare, 3.0 * s.coreShare);
            }
        }
    }

    BambiEncoderProcessor proc;
    proc.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    /*  Both renderers. A plugin window on macOS is drawn by CoreGraphics, which is what the native
        image type is; the software renderer is what a headless check paints into. They price the
        same drawing very differently -- a call per cell is cheap in one and not in the other. */
    juce::Image software(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true, juce::SoftwareImageType());
    juce::Image native(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true, juce::NativeImageType());
    //  A full repaint every frame, at the editor's frame rate (PluginEditor.cpp): an upper bound, since a
    //  frame repaints only the scene, the list and the meter. The scene here is this one instance alone.
    constexpr double kEditorFrameHz = 30.0;
    const auto timePaint = [&](const char* what, juce::Image& image, const std::function<void(int)>& before = {}) {
        std::vector<double> ms;
        for (int i = 0; i < 220; ++i) {
            if (before) before(i);  // outside the clock: what moves the scene is not what paints it
            juce::Graphics g(image);
            const auto t0 = Clock::now();
            editor->paintEntireComponent(g, true);
            const double t = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (i >= 20) ms.push_back(t);
        }
        std::sort(ms.begin(), ms.end());
        const double median = ms[ms.size() / 2];
        std::printf(
            "\neditor paint, %s (%d x %d)\n  median %.3f ms, max %.3f ms\n  a full repaint at %.0f frames/s: %.2f%% of "
            "a core per open window, at most\n",
            what, editor->getWidth(), editor->getHeight(), median, ms.back(), kEditorFrameHz,
            median * kEditorFrameHz / 1000.0 * 100.0);
    };
    timePaint("no region, software", software);
    timePaint("no region, native", native);
    //  A spot with its page open: edge, wash and both views, which is the scene at its most expensive.
    auto* encoder = dynamic_cast<BambiEncoderEditor*>(editor.get());
    encoder->chooseRegionKind(1);
    encoder->showSource(bambi::kNumSources - 1);
    timePaint("a spot region, its page open, software", software);
    timePaint("a spot region, its page open, native", native);
    /*  Turning: the region is somewhere else every frame, so nothing kept can be reused and both
        views sample it afresh. The worst case a rate or a held handle produces. */
    timePaint("the same region turning every frame, native", native, [&](int i) {
        if (auto* p = proc.hostParameter(bambi::parameterByKey("region1.yaw")))
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(i % 170)));
        encoder->tick();
    });
    std::printf("\n");
    /*  The region layer alone, one view at the scene's own size: sampled afresh every paint (a region
        a rate is turning), and drawn from what was kept (one standing still). Every kind, because
        what a sample costs is the kind's own arithmetic. */
    {
        const int side = bambi::ui::theme::metrics::scenePanel;
        juce::Image view(juce::Image::ARGB, side, side, true, juce::NativeImageType());
        const bambi::Viewport vp{side / 2.0, side / 2.0, side / 2.0 - 8.0, side / 2.0 - 8.0};
        std::printf("region layer, one %d px view, wash on, native\n", side);
        std::printf("  kind      projection   turning      kept\n");
        const char* kinds[] = {"", "spot", "band", "sectors", "dots"};
        for (int kind = 1; kind <= 4; ++kind)
            for (const auto projection : {bambi::Projection::Globe, bambi::Projection::Equirect}) {
                bambi::ui::RegionLayer layer;
                std::vector<bambi::ui::SceneRegion> regions(1);
                regions[0].region.kind = static_cast<bambi::RegionKind>(kind);
                regions[0].region.softness = 20.0 * bambi::kDeg2Rad;
                regions[0].open = true;
                const auto run = [&](bool turning) {
                    std::vector<double> ms;
                    for (int i = 0; i < 220; ++i) {
                        if (turning) regions[0].region.yaw = 0.01 * i;
                        juce::Graphics g(view);
                        const auto t0 = Clock::now();
                        layer.paint(g, projection, bambi::Camera{}, vp, regions);
                        const double t = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                        if (i >= 20) ms.push_back(t);
                    }
                    std::sort(ms.begin(), ms.end());
                    return ms[ms.size() / 2];
                };
                const double turning = run(true), kept = run(false);
                std::printf("  %-8s  %-9s  %7.3f ms  %7.3f ms\n", kinds[kind],
                            projection == bambi::Projection::Globe ? "globe" : "equirect", turning, kept);
            }
        std::printf("\n");
    }
    /*  A whole sphere, as an effect's window repaints it when a region turns: frame, graticule,
        labels, the region and its handles -- not the region layer alone. Two of these a frame. */
    {
        const int side = bambi::ui::theme::metrics::scenePanel;
        juce::Image view(juce::Image::ARGB, side, side, true, juce::NativeImageType());
        std::printf("a whole sphere view, %d px, a spot open and turning, native\n", side);
        for (const auto projection : {bambi::Projection::Globe, bambi::Projection::Equirect}) {
            for (const bool energy : {false, true}) {
                bambi::ui::ProbeView::State state;
                state.energyOn = energy;
                if (energy)
                    state.energy.prepare(3);  // the picture is built every paint, whether or not anything arrived
                double yaw = 0.0;
                bambi::ui::ProbeView sphere(state, projection, [](bambi::Vec3, bambi::ui::ProbeReply&) {}, [] {});
                bambi::ui::RegionHook hook;
                hook.regions = [&yaw](std::vector<bambi::ui::SceneRegion>& into) {
                    bambi::ui::SceneRegion r;
                    r.region.kind = bambi::RegionKind::Spot;
                    r.region.softness = 20.0 * bambi::kDeg2Rad;
                    r.region.yaw = yaw;
                    r.open = true;
                    into.push_back(r);
                };
                sphere.setRegionHook(hook);
                sphere.setBounds(0, 0, side, side);
                std::vector<double> ms;
                for (int i = 0; i < 220; ++i) {
                    yaw = 0.01 * i;
                    juce::Graphics g(view);
                    const auto t0 = Clock::now();
                    sphere.paintEntireComponent(g, true);
                    const double t = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                    if (i >= 20) ms.push_back(t);
                }
                std::sort(ms.begin(), ms.end());
                std::printf("  %-9s  energy %-3s  %7.3f ms\n",
                            projection == bambi::Projection::Globe ? "globe" : "equirect", energy ? "on" : "off",
                            ms[ms.size() / 2]);
            }
        }
        std::printf("\n");
    }
    editor.reset();
    return 0;
}
