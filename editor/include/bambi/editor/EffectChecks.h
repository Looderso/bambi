// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <vector>

#include "bambi/editor/CheckKit.h"
#include "bambi/host/BlockBench.h"
#include "bambi/host/LinkNode.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"

/*  The checks every field effect runs the same way: its layouts, that a render is independent of block size
    and repeats after a locate, its state, and the engine handoff. A suite fills an EffectCheckSpec and calls
    them in order, with its own checks between. */
namespace bambi::editor {

template <class Processor>
struct EffectCheckSpec {
    const char* tool{""};        ///< the check binary's name, as it prints it
    const char* whatIsInIt{""};  ///< "the echo", "the room": what makes the output differ from the input
    const ParamManifest* params{nullptr};
    Product foreignProduct{Product::Unknown};  ///< another product, whose state this one must refuse
    /// The suite's render of the test field (`renderField` with its own parameters).
    std::function<Channels(const Layout&, int block, double seconds, const RenderRun&)> render;

    juce::String stateKey;  ///< a parameter the state check moves, saves and reads back
    float stateValue{0.0f};
    float stateTolerance{0.05f};
    std::function<void(Processor&)> stateSetUp;  ///< anything that must be on for `stateKey` to matter

    juce::String drivenKey;                           ///< a parameter the input's level drives in the locate check
    juce::String drivenWhat;                          ///< how the check names it: "a tap's spin"
    std::function<void(PluginState&)> drivenRegions;  ///< regions turning at a rate in that check

    int processChannels{18};  ///< the buffer width a bare processBlock is handed

    Product product{Product::Unknown};
    const char* linkDirectory{""};  ///< a private link directory prefix, e.g. "/bambi.ec."
    juce::String plural;            ///< "Echoes", as the link check names two of them
    juce::String withArticle;       ///< "an Echo"
    juce::String remoteKey;         ///< a parameter the link check edits from another instance
    float remoteValue{0.0f};
    /// The suite's own link checks: after the two instances have joined, and at the end.
    std::function<void(Processor& one, Processor& two, const std::function<void(int)>& tick)> linkAfterJoin, linkAtEnd;
};

/// `--bench`: what a whole processBlock costs, with the energy picture off and on. Release builds only.
template <class Processor>
int benchEffect(const EffectCheckSpec<Processor>& spec) {
#if JUCE_DEBUG
    std::printf("\n  *** DEBUG BUILD -- these numbers are meaningless. ***\n");
#endif
    std::printf("%s --bench   (48 kHz, transport playing, noise on every input, defaults)\n\n", spec.tool);
    for (const bool looking : {false, true}) {
        std::printf("%s\n  order    ch  block      median         p99         max    one core\n",
                    looking ? "\nthe energy picture ON in a window" : "the energy picture off, as a window opens");
        for (const int order : {1, 3, 7})
            for (const int block : {128, 512}) {
                Processor proc;
                proc.wantEnergy(looking);
                const auto set = juce::AudioChannelSet::ambisonic(order);
                if (!proc.setBusesLayout(layout(set, juce::AudioChannelSet::stereo(), set))) {
                    std::printf("  %5d  layout refused\n", order);
                    continue;
                }
                host::printBlockStats(order, block, host::benchBlocks(proc, kCheckRate, block));
            }
    }
    return 0;
}

/// A field effect takes equal input and output widths at every order, with or without a sidechain.
template <class Processor>
void checkEffectLayouts() {
    std::printf("layouts a field effect accepts\n");
    Processor proc;
    const auto accepts = [&proc](const Layout& l) { return proc.checkBusesLayoutSupported(l); };
    for (const int order : {0, 1, 2, 3, 5, 7}) {
        const auto l = layout(amb(order), juce::AudioChannelSet::stereo(), amb(order));
        check(accepts(l), "equal widths, order " + juce::String(order) + ": " + describe(l));
    }
    //  Unlike the encoder, an effect never returns a different width than it was given.
    const auto wider = layout(amb(1), juce::AudioChannelSet::stereo(), amb(3));
    check(!accepts(wider), "unequal widths refused: " + describe(wider));
    const auto narrower = layout(amb(3), juce::AudioChannelSet::stereo(), amb(1));
    check(!accepts(narrower), "and the other way round: " + describe(narrower));
    Layout noSide;
    noSide.inputBuses.add(amb(3));
    noSide.inputBuses.add(juce::AudioChannelSet::disabled());
    noSide.outputBuses.add(amb(3));
    check(accepts(noSide), "a sidechain that is not connected: " + describe(noSide));
}

/// The field goes through and is changed, whatever the block size.
template <class Processor>
void checkEffectRenders(const EffectCheckSpec<Processor>& spec) {
    const auto l = layout(amb(3), juce::AudioChannelSet::stereo(), amb(3));

    std::printf("\nthe field goes through, and %s is in it\n", spec.whatIsInIt);
    {
        const auto out = spec.render(l, 128, 1.0, {});
        check(out.size() == 16, "sixteen channels out at order 3");
        check(energy(out) > 0.0, "and they are not silent");
        //  The check is that the field changed, which a bypass would fail.
        Channels dry(16);
        for (int c = 0; c < 16; ++c)
            for (int i = 0; i < static_cast<int>(kCheckRate); ++i)
                dry[static_cast<std::size_t>(c)].push_back(fieldSample(c, i));
        check(!identical(out, dry), "the output is not the input handed back");
    }

    std::printf("\nblock size decides nothing\n");
    {
        const auto reference = spec.render(l, 128, 1.0, {});
        for (const int block : {1, 64, 333, 1024})
            check(identical(reference, spec.render(l, block, 1.0, {})),
                  "block " + juce::String(block) + " renders the same samples as 128");
    }
}

/*  After a locate the render repeats: back to zero, to a sample the control hop does not divide, and with a
    feature driving a parameter and regions turning. */
template <class Processor>
void checkEffectLocates(const EffectCheckSpec<Processor>& spec) {
    std::printf("\na locate makes the render repeat\n");
    const auto l = layout(amb(3), juce::AudioChannelSet::stereo(), amb(3));
    constexpr int kBlock = 128;
    //  A host locates between blocks, so the jump sits on a block boundary.
    const auto at = static_cast<juce::int64>(0.5 * kCheckRate) / kBlock * kBlock;
    const auto repeatsAfter = [&](const Channels& once, const Channels& twice) {
        bool repeats = !once.empty() && once.size() == twice.size();
        const auto half = static_cast<std::size_t>(at);
        for (std::size_t c = 0; repeats && c < once.size(); ++c)
            for (std::size_t i = 0; i < half && repeats; ++i) repeats = once[c][i] == twice[c][half + i];
        return repeats;
    };
    {
        const auto once = spec.render(l, kBlock, 0.5, {});
        RenderRun back;
        back.locateAt = at;
        check(repeatsAfter(once, spec.render(l, kBlock, 1.0, back)),
              "after a jump back to zero, the same samples come out again");
    }
    {
        /*  To a timeline that is not a whole number of 256-sample control hops, and far enough to count as a
            locate: the grid is pinned to the timeline, so a bounce starting there steps where playback does. */
        const juce::int64 to = 4 * static_cast<juce::int64>(kCheckRate) + 77;
        RenderRun startThere;
        startThere.startAt = to;
        RenderRun jumpThere;
        jumpThere.locateAt = 23936;
        jumpThere.locateTo = to;
        const auto fromThere = spec.render(l, kBlock, 0.3, startThere);
        const auto located = spec.render(l, kBlock, 0.85, jumpThere);  // holds 23936 samples before the jump
        bool same = !fromThere.empty() && !located.empty() && fromThere.size() == located.size();
        const std::size_t after = located.empty() ? 0 : located[0].size() - 23936;
        for (std::size_t c = 0; same && c < fromThere.size(); ++c)
            for (std::size_t i = 0; i < std::min(fromThere[c].size(), after) && same; ++i)
                same = fromThere[c][i] == located[c][23936 + i];
        check(same, "a locate to a sample the hop does not divide lands on the same grid");

        //  And asked directly: where the processor's grid is after such a locate.
        Processor proc;
        proc.setBusesLayout(l);
        proc.prepareToPlay(kCheckRate, kBlock);
        host::TestPlayHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(18, kBlock);
        juce::MidiBuffer midi;
        head.time = to;
        buffer.clear();
        proc.processBlock(buffer, midi);
        const int expected = static_cast<int>((to + kBlock) % 256);
        check(proc.diagnostics().controlPhase.load() == expected,
              "and the grid sits where the timeline says: phase " +
                  juce::String(proc.diagnostics().controlPhase.load()) + ", expected " + juce::String(expected));
    }
    {
        //  With a feature driving a parameter, a locate must also reset the detectors.
        PluginState driven{*spec.params};
        driven.matrix.push_back(
            {MatrixTab::Features, 0, static_cast<ParamId>(spec.params->byKey(spec.drivenKey.toStdString())), 1.0});
        if (spec.drivenRegions) spec.drivenRegions(driven);
        RenderRun plain;
        plain.patch = &driven;
        RenderRun back = plain;
        back.locateAt = at;
        check(repeatsAfter(spec.render(l, kBlock, 0.5, plain), spec.render(l, kBlock, 1.0, back)),
              "and with the input's level driving " + spec.drivenWhat + ", it still repeats");
    }
}

/// A state is written, read back into another instance, and refused when another product wrote it.
template <class Processor>
void checkEffectState(const EffectCheckSpec<Processor>& spec) {
    std::printf("\nstate\n");
    const auto valueOf = [](juce::AudioProcessor& p, const juce::String& id) {
        auto* param = findParam(p, id);
        return param != nullptr ? param->convertFrom0to1(param->getValue()) : -1.0f;
    };
    Processor proc;
    if (spec.stateSetUp) spec.stateSetUp(proc);
    setParameter(proc, spec.stateKey, spec.stateValue);
    juce::MemoryBlock saved;
    proc.getStateInformation(saved);
    check(saved.getSize() > 0, "a state is written");

    Processor other;
    other.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    check(std::abs(valueOf(other, spec.stateKey) - spec.stateValue) < spec.stateTolerance,
          "and read back into another instance");

    //  This product's own keys with only the product tag changed, so what is refused is the tag.
    PluginState foreignState{*spec.params};
    const std::string foreign = saveState(spec.foreignProduct, *spec.params, foreignState);
    Processor third;
    setParameter(third, spec.stateKey, spec.stateValue);
    third.setStateInformation(foreign.data(), static_cast<int>(foreign.size()));
    check(std::abs(valueOf(third, spec.stateKey) - spec.stateValue) < spec.stateTolerance,
          "a state another product wrote is refused, leaving this one as it was");
}

/// Forty state commits against a running audio thread: every one adopted, none deferred, none freed there.
template <class Processor>
void checkEffectHandoff(const EffectCheckSpec<Processor>& spec) {
    std::printf("\nthe handoff frees nothing on the audio thread\n");
    const int freedBefore = Processor::engineSnapshotsFreedOnAudioThread();
    {
        Processor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kCheckRate, 128);
        host::TestPlayHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(18, 128);
        juce::MidiBuffer midi;
        for (int i = 0; i < 40; ++i) {
            PluginState st{*spec.params};
            st.regions[0].shape.kind = (i % 2 == 0) ? RegionKind::Spot : RegionKind::Band;
            proc.commitDocument(st, false);
            head.time = i * 128;
            buffer.clear();
            proc.processBlock(buffer, midi);
        }
        //  The last commit, not merely one: a retire queue that never drained would fill after sixteen and
        //  defer every adoption from then on.
        check(proc.diagnostics().stateSequence.load() == 40,
              "the audio thread adopted every one of forty commits, in order");
        check(proc.diagnostics().adoptionsDeferred.load() == 0,
              "and deferred none of them: the retire queue never filled");
    }
    check(Processor::engineSnapshotsFreedOnAudioThread() == freedBefore, "and freed none of it inside processBlock");
    check(Processor::liveEngineSnapshots() == 0, "every snapshot is accounted for at the end");
}

/// Two instances on a private link bus: they join, see each other, and edit each other's parameters and patch.
template <class Processor>
void checkEffectLinkBus(const EffectCheckSpec<Processor>& spec) {
    std::printf("\nthe link bus\n");
    //  A private session directory: the machine's real one may belong to a running DAW session.
    host::setLinkDirectoryNameForTests(std::string(spec.linkDirectory) + Uuid::generate().toString().substr(0, 8));

    const auto l = layout(amb(3), juce::AudioChannelSet::stereo(), amb(3));
    Processor one, two;
    for (auto* p : {&one, &two}) {
        p->setBusesLayout(l);
        p->prepareToPlay(kCheckRate, 128);
    }
    //  Ticked by hand: a headless check has no message loop to run the timer.
    const std::function<void(int)> tick = [&](int ticks) {
        for (int i = 0; i < ticks; ++i)
            for (auto* p : {&one, &two}) p->linkTick();
    };
    tick(host::LinkNode::kTickHz / 10 + 4);

    check(one.linkStatus().open && two.linkStatus().open, "two " + spec.plural + " join the bus");
    check(one.identity().session == two.identity().session, "and the same session, as a new instance does");
    check(!(one.identity().instance == two.identity().instance), "with ids of their own");

    LinkScene scene;
    one.updateScene(scene);
    const bool found = scene.find(two.identity().instance) != nullptr;
    const auto* peer = scene.find(two.identity().instance);
    check(found, "each sees the other in its scene");
    check(peer != nullptr && peer->product == spec.product,
          "and knows what it is: " + spec.withArticle + ", not an encoder");
    if (spec.linkAfterJoin) spec.linkAfterJoin(one, two, tick);
    if (!found) return;

    //  A remote parameter edit arrives as a gesture on the receiver's own host parameter, so it automates and
    //  undoes as a local one would.
    const auto id = static_cast<ParamId>(spec.params->byKey(spec.remoteKey.toStdString()));
    one.link().sendParameter(two.identity().instance, id, spec.remoteValue, kLinkGestureBegin);
    one.link().sendParameter(two.identity().instance, id, spec.remoteValue, kLinkGestureEnd);
    tick(3);
    const auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(two.getParameters()[static_cast<int>(id)]);
    check(ranged != nullptr && std::abs(ranged->convertFrom0to1(ranged->getValue()) - spec.remoteValue) < 0.05f,
          "a remote parameter edit reaches the other instance's own host parameter");

    //  A remote patch edit is state, so it lands as an undo step on the receiver.
    PluginState wanted = two.document().editing();
    wanted.regions[0].shape.kind = RegionKind::Band;
    LinkPatch packed;
    packPatch(wanted, packed);
    one.link().sendPatchEdit(two.identity().instance, packed, "region kind", {}, true);
    tick(3);
    check(two.document().editing().regions[0].shape.kind == RegionKind::Band, "a remote patch edit reaches it too");
    check(two.document().canUndo(), "and lands as an undo step, which is why undo is here at all");
    check(two.document().undo() && two.document().editing().regions[0].shape.kind != RegionKind::Band,
          "so the other window can undo it");
    if (spec.linkAtEnd) spec.linkAtEnd(one, two, tick);
}

/*  `rates.retrigger`, through the processor, which reads the transport: set to restart, a turn holds while
    stopped and a locate takes it back; set to continue, it runs stopped and a locate leaves it. */
template <class Processor>
void checkEffectRetrigger(const EffectCheckSpec<Processor>& spec) {
    std::printf("\nrates.retrigger: a turn is tied to playback, or runs on\n");
    Processor proc;
    proc.prepareToPlay(kCheckRate, 128);
    host::TestPlayHead head;
    proc.setPlayHead(&head);
    const auto set = [&](const char* key, float value) {
        if (auto* p = proc.hostParameter(static_cast<ParamId>(spec.params->byKey(key))))
            p->setValueNotifyingHost(p->convertTo0to1(value));
    };
    const auto turn = [&] {
        return static_cast<double>(proc.diagnostics().regionTurn[0].load(std::memory_order_relaxed));
    };
    juce::AudioBuffer<float> buffer(spec.processChannels, 128);
    juce::MidiBuffer midi;
    const auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
            if (head.playing) head.time += buffer.getNumSamples();
        }
    };
    set("region1.yaw_rate", 60.0f);

    head.playing = false;
    run(100);
    check(std::abs(turn()) < 1e-6, "set to restart, a stopped transport holds the turn");
    head.playing = true;
    run(100);
    const double played = turn();
    check(played > 0.2, "and it turns while it plays");
    head.time = 0;  // a locate
    run(1);
    check(turn() < 0.05, "and a locate takes it back, so a render repeats");

    set("rates.retrigger", 1.0f);
    head.playing = false;
    run(100);
    const double ran = turn();
    check(ran > 0.2, "set to continue, it runs with the transport stopped");
    head.playing = true;
    head.time = 480000;  // a locate
    run(1);
    check(turn() >= ran, "and a locate leaves it where it was");
}

/*  An effect has no source position, so the matrix does not offer the region tab (it would read zero), and a
    plugin put on that tab falls back to one its bar draws. */
template <class Processor, class Editor>
void checkRegionIsNotASource() {
    std::printf("\nthe region is not a source here\n");
    Processor proc;
    proc.prepareToPlay(kCheckRate, 128);
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* ed = dynamic_cast<Editor*>(editor.get());
    if (ed == nullptr) {
        check(false, "no editor: the region tab check cannot run");
        return;
    }
    const auto offered = ed->matrixTabsOffered();
    check(std::find(offered.begin(), offered.end(), MatrixTab::Region) == offered.end(),
          "the matrix does not offer the region tab");
    check(offered.size() == 3, "it offers the other three");

    ed->selectMatrixTab(MatrixTab::Region);
    ed->paintNow();
    check(ed->shownMatrixTab() != MatrixTab::Region,
          "and a plugin put on that tab falls back to one its bar actually draws");
}

/*  The globe's strip snaps to the shared view presets: a click moves the camera and lights the chip, and a
    drag that orbits away leaves none lit. */
template <class Processor, class Editor>
void checkEffectViewPresets() {
    std::printf("\nthe globe's view presets, in every plugin's strip\n");
    Processor proc;
    proc.prepareToPlay(kCheckRate, 128);
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    auto* ed = dynamic_cast<Editor*>(editor.get());
    if (ed == nullptr) {
        check(false, "no editor: the view preset checks cannot run");
        return;
    }
    const auto sameView = [](Camera a, Camera b) {
        return juce::exactlyEqual(a.yaw, b.yaw) && juce::exactlyEqual(a.pitch, b.pitch);
    };
    const auto top = cameraFor(ViewPreset::Top);
    check(!sameView(ed->camera(), top), "the globe does not open on the top view");
    check(ed->clickViewPreset(0) && ed->viewPreset() == ViewPreset::Top && sameView(ed->camera(), top),
          "clicking `top` in the globe's strip puts the camera there and lights the chip");
    check(ed->clickViewPreset(2) && ed->viewPreset() == ViewPreset::Side &&
              sameView(ed->camera(), cameraFor(ViewPreset::Side)),
          "and `side` the same");
    ed->dragProbe(0.0, 0.0, 0.35, 0.2);
    check(ed->viewPreset() == ViewPreset::Free && !sameView(ed->camera(), cameraFor(ViewPreset::Side)),
          "a drag that orbits the globe away leaves no preset lit");
}

}  // namespace bambi::editor
