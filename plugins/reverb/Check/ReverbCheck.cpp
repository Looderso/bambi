// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-reverb-check — Reverb's host-contract properties, on the shipped sources, with no host.
//
//  The same questions Echo's check asks, asked again of the third plugin. That is the point of it:
//  both field effects run the same bus policy, transport watch, control grid, field input and
//  state handoff, and a suite whose plugins locate or hand over state differently would put two
//  different renders in one bounce. Reverb's own answers are its engine's:
//
//    - a field effect accepts equal in and out widths and refuses unequal ones
//    - output does not depend on the host's block size -- an offline bounce and realtime playback
//      use different ones, so this is the bounce check, bit for bit, with the transport running
//    - a state round-trips, and a state another product wrote is refused rather than half-read
//    - the engine snapshot is never freed on the audio thread
//    - a locate makes the render repeat: a bounce matches what was heard
//
//  exit 0 every check passes, 1 a check failed

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

#include "../../echo/Source/PluginProcessor.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "bambi/editor/CheckKit.h"
#include "bambi/editor/EffectChecks.h"
#include "bambi/editor/PresetChecks.h"
#include "bambi/editor/RegionChecks.h"
#include "bambi/host/BlockBench.h"
#include "bambi/host/LinkNode.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/mod/matrix.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/reverb/params.hpp"
#include "bambi/ui/RegionClipboard.h"
#include "bambi/ui/Strokes.h"

namespace {
constexpr double kRate = bambi::editor::kCheckRate;
using Processor = BambiReverbProcessor;

using bambi::editor::amb;
using bambi::editor::Channels;
using bambi::editor::check;
using bambi::editor::checkFailures;
using bambi::editor::checkVerdict;
using bambi::editor::describe;
using bambi::editor::energy;
using bambi::editor::fieldSample;
using bambi::editor::identical;
using bambi::editor::Layout;
using bambi::editor::layout;
using bambi::editor::setParameter;
using PlayingHead = bambi::host::TestPlayHead;

/*  `seconds` of the test field through a fresh processor, in blocks of `block`: the main output. A locate can
    be asked for partway, which is what makes the repeat check possible. */
Channels render(const Layout& l, int block, double seconds, const std::string& state = {}, juce::int64 locateAt = -1,
                juce::int64 locateTo = 0, juce::int64 startAt = 0, const bambi::PluginState* patch = nullptr) {
    const auto setUp = [](Processor& p) {
        setParameter(p, "room.size", 20.0f);
        setParameter(p, "room.decay", 1.9f);
        setParameter(p, "output.wet", 0.0f);
        setParameter(p, "output.dry", -6.0f);
        setParameter(p, "region1.yaw_rate", 120.0f);
        setParameter(p, "region2.pitch_rate", 90.0f);
    };
    return bambi::editor::renderField<Processor>(l, block, seconds, setUp, {state, locateAt, locateTo, startAt, patch});
}

bambi::editor::EffectCheckSpec<Processor> effectSpec() {
    bambi::editor::EffectCheckSpec<Processor> spec;
    spec.tool = "bambi-reverb-check";
    spec.whatIsInIt = "the room";
    spec.params = &bambi::reverbParams();
    spec.foreignProduct = bambi::Product::Echo;
    spec.render = [](const Layout& l, int block, double seconds, const bambi::editor::RenderRun& run) {
        return render(l, block, seconds, run.state, run.locateAt, run.locateTo, run.startAt, run.patch);
    };
    spec.stateKey = "room.size";
    spec.stateValue = 31.0f;
    spec.stateTolerance = 0.05f;
    spec.stateSetUp = nullptr;
    spec.drivenKey = "room.decay";
    spec.drivenWhat = "the room's decay";
    spec.drivenRegions = [](bambi::PluginState& st) {
        st.regions[0].shape.kind = bambi::RegionKind::Spot;
        st.regions[1].shape.kind = bambi::RegionKind::Band;
    };
    spec.processChannels = 34;
    spec.product = bambi::Product::Reverb;
    spec.linkDirectory = "/bambi.rc.";
    spec.plural = "Reverbs";
    spec.withArticle = "a Reverb";
    spec.remoteKey = "room.decay";
    spec.remoteValue = 4.5f;
    //  A name typed on the settings page is republished at once, so every other window shows it.
    spec.linkAfterJoin = [](Processor& one, Processor& two, const std::function<void(int)>& tick) {
        std::unique_ptr<juce::AudioProcessorEditor> twosEditor(two.createEditor());
        if (auto* ed = dynamic_cast<BambiReverbEditor*>(twosEditor.get())) {
            ed->openSettings(true);
            ed->typeInstanceName("plate, long");
        }
        tick(bambi::host::LinkNode::kTickHz / 10 + 4);
        bambi::LinkScene scene;
        one.updateScene(scene);
        const auto* renamed = scene.find(two.identity().instance);
        check(renamed != nullptr && renamed->st.labelString() == "plate, long",
              "a name typed on one instance's settings page is what another instance reads on the bus");
    };
    //  What a bounce should do is state, so it travels in a patch like anything else.
    spec.linkAtEnd = [](Processor& one, Processor& two, const std::function<void(int)>& tick) {
        bambi::PluginState wanted = two.document().editing();
        wanted.renderQuality = bambi::RenderQuality::Same;
        bambi::LinkPatch packed;
        bambi::packPatch(wanted, packed);
        one.link().sendPatchEdit(two.identity().instance, packed, "render quality", {}, true);
        tick(3);
        check(two.renderQuality() == bambi::RenderQuality::Same, "and what a bounce should do travels with it");
    };
    return spec;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;

    /*  `--bench`: what a whole processBlock costs, as a host drives it. The engine alone is
        `bambi-bench`'s; this is the plugin round it. Release only -- a Debug number means nothing. */
    const auto spec = effectSpec();
    if (argc > 1 && juce::String(argv[1]) == "--bench") return bambi::editor::benchEffect(spec);

    std::printf("bambi-reverb-check\n\n");

    bambi::editor::checkEffectLayouts<Processor>();
    bambi::editor::checkEffectRenders(spec);
    bambi::editor::checkEffectLocates(spec);

    {
        /*  Reverb is the only plugin with two regions -- a send on the virtual sources and a return
            on the lines -- and nothing above would notice if the processor handed the send's shape
            to both. Two renders that differ only in the return's kind must differ in their output;
            with the return taken from slot 0 they would be the same render twice. */
        const auto l = layout(amb(3), juce::AudioChannelSet::stereo(), amb(3));
        const auto withKind = [&](bambi::RegionKind returnKind) {
            bambi::PluginState st{bambi::reverbParams()};
            st.regions[0].shape.kind = bambi::RegionKind::Spot;
            st.regions[1].shape.kind = returnKind;
            return render(l, 128, 0.4, {}, -1, 0, 0, &st);
        };
        const auto everywhere = withKind(bambi::RegionKind::Everywhere);
        const auto spot = withKind(bambi::RegionKind::Spot);
        check(!everywhere.empty() && !identical(everywhere, spot),
              "the RETURN region is its own: slot 1, not the send's shape used twice");
    }

    {
        /*  A bounce may be worth more than playback: `quality.playing` is what the user rides, and
            offline the engine goes to the full tail instead. Nothing else here can see that,
            because everything else renders realtime. */
        const auto l = layout(amb(3), juce::AudioChannelSet::stereo(), amb(3));
        const auto linesWhen = [&](bool offline) {
            BambiReverbProcessor proc;
            proc.setBusesLayout(l);
            proc.setNonRealtime(offline);
            proc.prepareToPlay(kRate, 128);
            setParameter(proc, "quality.playing", 0.0f);  // efficient while playing
            PlayingHead head;
            proc.setPlayHead(&head);
            juce::AudioBuffer<float> buffer(18, 128);
            juce::MidiBuffer midi;
            buffer.clear();
            proc.processBlock(buffer, midi);
            return proc.tailLines();
        };
        const int playing = linesWhen(false), bouncing = linesWhen(true);
        check(playing > 0 && bouncing > playing,
              "set to efficient, a bounce still gets the full tail: " + juce::String(playing) + " lines playing, " +
                  juce::String(bouncing) + " offline");

        /*  And that is a choice, which is why it is state: asked to leave a bounce alone, the
            offline render keeps the efficient tail. Without this the check above passes on a plugin
            that ignores the setting and always goes full. */
        BambiReverbProcessor same;
        same.setBusesLayout(l);
        same.setNonRealtime(true);
        same.prepareToPlay(kRate, 128);
        same.setRenderQuality(bambi::RenderQuality::Same);
        setParameter(same, "quality.playing", 0.0f);
        PlayingHead head2;
        same.setPlayHead(&head2);
        juce::AudioBuffer<float> b2(18, 128);
        juce::MidiBuffer m2;
        b2.clear();
        same.processBlock(b2, m2);
        check(same.tailLines() == playing, "and asked to leave a bounce alone, it keeps the efficient tail (" +
                                               juce::String(same.tailLines()) + ")");

        //  It is state, so it is saved. A setting a user makes and loses on reload is not a setting.
        juce::MemoryBlock saved;
        same.getStateInformation(saved);
        BambiReverbProcessor reloaded;
        reloaded.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        check(reloaded.renderQuality() == bambi::RenderQuality::Same, "and it survives a save and a load");
    }

    bambi::editor::checkEffectState(spec);
    bambi::editor::checkEffectHandoff(spec);

    bambi::editor::checkEffectLinkBus(spec);

    std::printf("\nthree products, one session\n");
    {
        /*  One bus carries all three, the scene is kind-agnostic, and an edit never crosses a
            kind -- because a parameter position means a different key in each plugin, so
            reinterpreting one would move the wrong control. */
        bambi::host::setLinkDirectoryNameForTests("/bambi.mix." + bambi::Uuid::generate().toString().substr(0, 8));

        BambiReverbProcessor reverb;
        BambiEchoProcessor echo;
        reverb.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        echo.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        reverb.prepareToPlay(kRate, 128);
        echo.prepareToPlay(kRate, 128);
        for (int i = 0; i < bambi::host::LinkNode::kTickHz / 10 + 4; ++i) {
            reverb.linkTick();
            echo.linkTick();
        }

        check(reverb.identity().session == echo.identity().session, "a Reverb and an Echo join the same session");

        bambi::LinkScene scene;
        reverb.updateScene(scene);
        const auto* asEcho = scene.find(echo.identity().instance);
        check(asEcho != nullptr && asEcho->product == bambi::Product::Echo,
              "and the Reverb's scene knows the other one is an Echo");

        /*  An edit addressed across kinds is refused, not reinterpreted. `room.decay` is position 2
            in Reverb; whatever sits at position 2 in Echo is a different control entirely. */
        const auto id = static_cast<bambi::ParamId>(bambi::reverbParams().byKey("room.decay"));
        auto* target = dynamic_cast<juce::RangedAudioParameter*>(echo.getParameters()[static_cast<int>(id)]);
        const float before = target == nullptr ? 0.0f : target->getValue();
        reverb.link().sendParameter(echo.identity().instance, id, 4.5f, bambi::kLinkGestureEnd);
        for (int i = 0; i < 3; ++i) {
            reverb.linkTick();
            echo.linkTick();
        }
        check(target != nullptr && juce::exactlyEqual(target->getValue(), before),
              "and an edit sent across kinds moves nothing");
    }

    std::printf("\nthe editor\n");
    {
        BambiReverbProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* rev = dynamic_cast<BambiReverbEditor*>(editor.get());
        check(rev != nullptr, "Reverb has an editor, and it opens");
        if (rev == nullptr) {
            std::printf("\n%s\n", "SOME CHECKS FAILED");
            return 1;
        }

        /*  Every control is reachable on one of the three tabs. Named by key rather than counted,
            so a control quietly left off a tab is named when it fails. */
        const auto& m = bambi::reverbParams();
        juce::String missing;
        int reached = 0;
        for (int i = 0; i < m.size(); ++i) {
            const auto key = std::string(m[i].key);
            //  the generators and the matrix's amounts are the shared engine's, not this panel's
            if (key.starts_with("lfo") || key.starts_with("env") ||
                (key.starts_with("mod.") && !key.starts_with("mod.amount.region")) || key == "level.release" ||
                key == "sc_level.release"     // on Level's own page: checked below
                || key == "quality.playing")  // on the settings page, beside when rendering: checked there
                continue;
            const auto id = static_cast<bambi::ParamId>(i);
            bool found = false;
            //  room, send, return: a slot is its tab
            for (int tab = 0; tab < 3 && !found; ++tab) {
                rev->showTab(tab);
                found = rev->drawnSomewhere(id);
                const int slot = tab - 1;
                /*  A shape tile belongs to one kind: a band's elevation is not drawn while a spot is
                    open, and should not be. So every kind is tried, which is what a user does by
                    clicking through them. The five shapes and clouds; custom is a flag over a shape
                    and has no tile of its own. */
                for (int sub = 0; slot >= 0 && sub < 2 && !found; ++sub) {  // shape, transform
                    rev->setRegionTab(sub);
                    for (int kind = 0; kind < 6 && !found; ++kind) {
                        rev->chooseRegionKind(slot, kind);
                        found = rev->drawnSomewhere(id);
                    }
                }
                rev->setRegionTab(0);
            }
            if (found)
                ++reached;
            else if (missing.isEmpty())
                missing = juce::String(key);
        }
        check(missing.isEmpty(), "every control of the room, its slots and its I/O is reachable (" +
                                     juce::String(reached) +
                                     (missing.isEmpty() ? juce::String() : ", first missing " + missing) + ")");

        //  A tab a slot: the send's tab shows the send alone, the return's the return.
        rev->chooseRegionKind(0, static_cast<int>(bambi::RegionKind::Spot));
        rev->chooseRegionKind(1, static_cast<int>(bambi::RegionKind::Spot));
        rev->setRegionTab(1);
        rev->showTab(1);
        check(rev->drawnSomewhere(static_cast<bambi::ParamId>(m.byKey("region1.yaw"))) &&
                  !rev->drawnSomewhere(static_cast<bambi::ParamId>(m.byKey("region2.yaw"))),
              "the send tab shows the send alone");
        rev->showTab(2);
        check(rev->drawnSomewhere(static_cast<bambi::ParamId>(m.byKey("region2.yaw"))) &&
                  !rev->drawnSomewhere(static_cast<bambi::ParamId>(m.byKey("region1.yaw"))),
              "and the return tab the return");

        /*  Every value on the room tab can actually be moved. Reachable is not the same as usable:
            a tile that is drawn, registers its box and takes a drag that changes nothing is exactly
            what "I cannot change the dry" looks like. */
        rev->showTab(0);
        for (const char* key :
             {"output.dry", "output.wet", "output.pre_delay", "room.size", "room.decay", "input.low_cut"}) {
            const auto id = static_cast<bambi::ParamId>(m.byKey(key));
            auto* param = proc.hostParameter(id);
            const float before = param == nullptr ? -1.0f : param->getValue();
            rev->dragTile(id, -60.0f);  // upward: every one of these opens below its top
            const float after = param == nullptr ? -1.0f : param->getValue();
            check(param != nullptr && !juce::exactlyEqual(after, before),
                  juce::String(key) + " moves when it is dragged (" + juce::String(before, 3) + " -> " +
                      juce::String(after, 3) + ")");
        }
        /*  One turn wraps: a region's yaw dragged up past 180 comes in again at -180, and a value
            with real ends stops at its top. Catches: the drag clamps every value, or wraps every value. */
        {
            const auto yaw = static_cast<bambi::ParamId>(m.byKey("region1.yaw"));
            const auto size = static_cast<bambi::ParamId>(m.byKey("room.size"));
            auto* yawParam = proc.hostParameter(yaw);
            auto* sizeParam = proc.hostParameter(size);
            rev->showTab(1);
            rev->setRegionTab(1);
            rev->chooseRegionKind(0, static_cast<int>(bambi::RegionKind::Spot));
            if (yawParam != nullptr) yawParam->setValueNotifyingHost(yawParam->convertTo0to1(170.0f));
            rev->tick();  // the window reads the host's value on its next frame
            rev->paintNow();
            //  20 degrees of a 360-degree range, over the pixels a whole range takes
            rev->dragTile(yaw, -20.0f / 360.0f * bambi::ui::theme::controls::dragPixels);
            const float landed = yawParam == nullptr ? 0.0f : yawParam->convertFrom0to1(yawParam->getValue());
            check(std::abs(landed + 170.0f) < 1.0f,
                  "a region's yaw dragged up past 180 comes in at -180 (" + juce::String(landed, 1) + " degrees)");
            rev->showTab(0);
            rev->paintNow();
            rev->dragTile(size, -3.0f * bambi::ui::theme::controls::dragPixels);
            check(sizeParam != nullptr && juce::exactlyEqual(sizeParam->getValue(), 1.0f),
                  "and the room's size, which has real ends, stops at its top");
        }

        /*  A choice steps on a click and does not drag: at its top a drag upward moves nothing,
            which is the right behaviour and not a stuck control. */
        {
            const auto id = static_cast<bambi::ParamId>(m.byKey("region1.side"));
            auto* param = proc.hostParameter(id);
            const float before = param == nullptr ? -1.0f : param->getValue();
            rev->showTab(1);
            rev->showSlot(0);
            rev->chooseRegionKind(0, 1);  // a spot: a side to choose
            rev->setRegionTab(1);
            rev->touchParameter(id);  // scrolls the regions tab down to reach it: the room tab below starts at its top
            check(param != nullptr && !juce::exactlyEqual(param->getValue(), before),
                  "a choice steps when it is clicked");
            if (param != nullptr) param->setValueNotifyingHost(before);  // and put back, for what follows
            rev->chooseRegionKind(0, 0);
            rev->setRegionTab(0);
        }

        /*  A preset moves the room through the host's own parameters -- so it automates and undoes
            as turning the knobs by hand would -- and leaves the mix alone. */
        rev->showTab(0);
        auto* decay = proc.hostParameter(static_cast<bambi::ParamId>(m.byKey("room.decay")));
        auto* wet = proc.hostParameter(static_cast<bambi::ParamId>(m.byKey("output.wet")));
        const float wetBefore = wet == nullptr ? 0.0f : wet->getValue();
        const float decayBefore = decay == nullptr ? 0.0f : decay->getValue();
        check(rev->clickPreset(5), "the cathedral preset is clickable");  // the longest of the six
        check(decay != nullptr && decay->getValue() > decayBefore,
              "and it lengthens the decay through the host's own parameter");
        check(wet != nullptr && juce::exactlyEqual(wet->getValue(), wetBefore),
              "and leaves the wet level alone: a preset is the room, not the mix");
    }

    std::printf("\nthe probe answers with the room's reflections\n");
    {
        BambiReverbProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* rev = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (rev == nullptr) {
            std::printf("\nSOME CHECKS FAILED\n");
            return 1;
        }

        bambi::ui::ProbeReply reply;
        rev->answerProbe({1.0, 0.0, 0.0}, reply);
        const auto& marks = reply.marks;
        check(!marks.empty(), "pointing at a direction answers with reflections (" +
                                  juce::String(static_cast<int>(marks.size())) + ")");
        bool onSphere = true, weighted = true;
        for (const auto& mark : marks) {
            onSphere = onSphere && std::abs(std::sqrt(bambi::dot(mark.direction, mark.direction)) - 1.0) < 1e-6;
            weighted = weighted && mark.weight > 0.0 && mark.weight <= 1.0;
        }
        check(onSphere, "each arrives from a direction");
        check(weighted, "each carries a weight, and none is louder than the source");

        //  The answer is the room's: a different room answers differently.
        bambi::ui::ProbeReply biggerReply;
        rev->showTab(0);
        rev->clickPreset(5);  // cathedral: far larger than the default
        /*  As the timer does. The probe answers from the editor's copy of the patch, which a tick
            refreshes; in a window that happens 30 times a second, so the picture lags a frame. */
        rev->tick();
        rev->answerProbe({1.0, 0.0, 0.0}, biggerReply);
        const auto& bigger = biggerReply.marks;
        bool moved = bigger.size() != marks.size();
        for (std::size_t i = 0; !moved && i < marks.size(); ++i)
            moved = bambi::dot(marks[i].direction, bigger[i].direction) < 0.999;
        check(moved, "and a different room answers differently: the picture is the plugin's own");
    }

    bambi::editor::checkRegionIsNotASource<Processor, BambiReverbEditor>();
    bambi::editor::checkEffectViewPresets<Processor, BambiReverbEditor>();

    std::printf("\nwhat moves on its own is redrawn, in an effect as in the encoder\n");
    {
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed == nullptr) {
            check(false, "no editor: the redraw checks cannot run");
            return 1;
        }
        ed->chooseRegionKind(0, static_cast<int>(bambi::RegionKind::Spot));
        ed->showTab(1);
        ed->tick();
        ed->paintNow();

        int asked = ed->regionRepaintsAsked();
        for (int i = 0; i < 5; ++i) ed->tick();
        check(ed->regionRepaintsAsked() == asked, "a region standing still asks for no repaint");
        /*  A sphere's graticule is a handful of strokes, not one a segment: a line drawn for each
            of about a thousand segments costs 11.6 ms a frame for the two views, which a host's
            meter shows while the plugin's own figure stands still. Two weights, a near and a far
            path each, two views: sixteen at most. Catches the graticule going back to a call a
            segment, which draws the identical picture. */
        {
            ed->paintNow();
            const int before = bambi::ui::strokesIssued();
            ed->paintNow();
            const int strokes = bambi::ui::strokesIssued() - before;
            check(strokes > 0 && strokes <= 16,
                  "both spheres' graticules reach the screen in a handful of strokes (" + juce::String(strokes) + ")");
        }

        /*  A level moved off its page keeps the way to be modulated: a click on it opens its
            matrix row, as a click on a tile does. Catches the column taking drags and no click,
            which leaves dry with no way to a row at all. */
        {
            const auto dry = static_cast<bambi::ParamId>(bambi::reverbParams().byKey("output.dry"));
            check(ed->touchParameter(dry) && ed->provisionalRow() == dry,
                  "a click on dry in the level column opens its matrix row");
        }
        //  the same footer the encoder has
        check(ed->footerReadout().contains("order") && ed->footerReadout().contains("dsp"),
              "the footer reads the order and this plugin's own load (" + ed->footerReadout() + ")");
        /*  With another instance's name in front, the readout is wider than what the two switches
            leave -- Reverb has two switches where Echo has one. Catches the name kept and the load
            cut when the readout is trimmed from the end. */
        ed->footer().setStatus("the long name of another reverb bus on a big session", 3, 5.83f);
        ed->paintNow();
        check(ed->footer().readoutFits() && ed->footer().drawnReadout().contains("dsp"),
              "showing another instance with a long name, the footer still draws the load whole (" +
                  ed->footer().drawnReadout() + ")");

        /*  A rate turns it on the audio thread, with nothing on screen touched. Catches the views
            not being asked once a frame, which left a turning region frozen until something else
            happened to repaint the sphere. */
        if (auto* p = proc.hostParameter(static_cast<bambi::ParamId>(bambi::reverbParams().byKey("region1.yaw_rate"))))
            p->setValueNotifyingHost(p->convertTo0to1(60.0f));
        //  playing, and moving on: set to restart, a rate turns only while the transport runs
        PlayingHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(34, 128);
        juce::MidiBuffer midi;
        for (int b = 0; b < 100; ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
            head.time += buffer.getNumSamples();
        }
        ed->tick();
        check(ed->regionRepaintsAsked() > asked, "and one a rate has turned asks for one, both views");
        const auto turned = static_cast<double>(proc.diagnostics().regionTurn[0].load(std::memory_order_relaxed));
        /*  And it is drawn turned: the region the last paint drew, not the engine's counter. Catches
            the scene drawing a region from its parameters as set, which the encoder's did. */
        check(turned > 0.2 && std::abs(ed->sceneRegionYaw(0) - turned) < 0.05,  // 100 blocks at 60 deg/s is 0.28
              "and the scene draws it where the rate has turned it to (" + juce::String(ed->sceneRegionYaw(0), 2) +
                  " against " + juce::String(turned, 2) + ")");

        /*  A source's live output moves with no parameter changing, so no refresh ever reports it.
            Catches the effects' tick not asking for the live part, which the encoder's always did. */
        ed->showSource(bambi::sourceSlot(bambi::MatrixTab::Generators, 0));
        ed->tick();
        ed->paintNow();
        ed->tick();  // settles whatever the page opening changed
        const int live = ed->liveRepaintsAsked();
        const int heads = ed->matrixHeaderRepaintsAsked();
        ed->tick();
        ed->tick();
        check(ed->liveRepaintsAsked() == live + 2, "an open LFO page redraws its live output every frame");
        //  Catches the shared frame losing the line: the column heads' live values then stand still.
        check(ed->matrixHeaderRepaintsAsked() == heads + 2,
              "and the matrix's column heads, where every source's moves");
    }

    std::printf("\nan envelope's trigger learns its note, in an effect as in the encoder\n");
    {
        /*  Arm, catch and take are the shared processor's. Through the button, as a user does it:
            click `listen` on env 1's trigger page, play a note, let the bus tick. Catches the
            effects' render never looking for the note, the tick never taking it, and the button
            staying inert. */
        BambiReverbProcessor proc;
        const auto first = juce::AudioChannelSet::ambisonic(1);
        proc.setBusesLayout(layout(first, juce::AudioChannelSet::disabled(), first));
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 256);
        proc.prepareToPlay(kRate, 256);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed != nullptr) {
            ed->showSource(bambi::sourceSlot(bambi::MatrixTab::Generators, bambi::kNumLfos));  // env 1
            ed->setEnvelopeTab(1);                                                             // trigger
            ed->tick();
            check(ed->namedDrawn("learn note"), "the listen button is live on an effect's trigger page");
            const bool clicked = ed->clickNamed("learn note");
            check(clicked && proc.noteLearnArmed() == 0, "clicking it arms env 1");

            juce::AudioBuffer<float> buffer(4, 256);
            buffer.clear();
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(7, 61, 0.9f), 40);
            midi.addEvent(juce::MidiMessage::noteOn(8, 70, 0.9f), 90);  // only the first note-on counts
            proc.processBlock(buffer, midi);
            proc.linkTick();
            const auto& trigger = proc.document().editing().envTriggers[0];
            check(trigger.noteLow == 61 && trigger.noteHigh == 61 && trigger.channel == 7,
                  "the next note played is the trigger's: note " + juce::String(trigger.noteLow) + ", channel " +
                      juce::String(trigger.channel));
            check(proc.noteLearnArmed() == -1, "and it stops listening once it has one");
            check(proc.document().undo() && proc.document().editing().envTriggers[0].noteLow != 61,
                  "and the learnt note is an edit: undo takes it back");

            /*  A window that closes while an envelope is listening stops it: nobody is left to see
                that it is armed, and the next note played -- a minute later, for another reason --
                would silently retarget the envelope. Catches the shared window not doing that. */
            ed->tick();
            const bool armedAgain = ed->clickNamed("learn note") && proc.noteLearnArmed() == 0;
            editor.reset();
            check(armedAgain && proc.noteLearnArmed() == -1, "closing the window stops an envelope listening");
        }
    }

    std::printf("\nonly the slot being edited takes the handles\n");
    {
        /*  Both of Reverb's regions are drawn, and one is open: the one whose tab is showing.
            Catches every slot opening at once -- two washes and two sets of handles over each
            other -- which no check saw when the mutation was tried on the shared code. */
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        proc.document().edit("two regions", [](bambi::PluginState& st) {
            st.regions[0].shape.kind = bambi::RegionKind::Spot;
            st.regions[1].shape.kind = bambi::RegionKind::Band;
        });
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed != nullptr) {
            ed->tick();
            ed->showTab(1);
            ed->showSlot(0);
            auto drawn = ed->sceneRegions();
            check(drawn.size() == 2 && drawn[0].open && !drawn[1].open,
                  "the send is open while its slot is shown, the return is an edge");
            ed->showSlot(1);
            drawn = ed->sceneRegions();
            check(drawn.size() == 2 && !drawn[0].open && drawn[1].open, "and the other way round");
            ed->showTab(0);
            drawn = ed->sceneRegions();
            check(drawn.size() == 2 && !drawn[0].open && !drawn[1].open, "and neither, off the regions tab");
        }
    }

    std::printf("\nthe channels past the order are not the field, and are cleared\n");
    {
        /*  Eight channels carry order 1 and four spare. A host that processes in place hands the
            spare ones over still holding whatever arrived; left alone, that is audio this plugin
            never processed, passed straight to whatever sums the bus. Catches the clear going
            missing on a channel count no order fills exactly. */
        BambiReverbProcessor proc;
        const auto wide = juce::AudioChannelSet::discreteChannels(8);
        const bool accepted = proc.setBusesLayout(layout(wide, juce::AudioChannelSet::disabled(), wide));
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.setRateAndBufferSizeDetails(kRate, 256);
        proc.prepareToPlay(kRate, 256);
        juce::AudioBuffer<float> buffer(8, 256);
        juce::MidiBuffer midi;
        float spare = 0.0f, field = 0.0f;
        for (juce::int64 at = 0; at < 8192; at += 256) {
            head.time = at;
            for (int c = 0; c < 8; ++c)
                for (int i = 0; i < 256; ++i)
                    buffer.setSample(
                        c, i,
                        0.25f * static_cast<float>(std::sin(6.2831853 * 300.0 * static_cast<double>(at + i) / kRate)));
            proc.processBlock(buffer, midi);
            for (int c = 0; c < 8; ++c)
                (c < 4 ? field : spare) = std::max(c < 4 ? field : spare, buffer.getMagnitude(c, 0, 256));
        }
        check(accepted && field > 0.01f, "an eight-channel bus renders order 1 in its first four");
        check(juce::exactlyEqual(spare, 0.0f), "and the four past it leave silent, whatever arrived in them");
    }

    std::printf("\nthe detector: a field's level, and how long it takes to fall\n");
    {
        /*  First-order field, W X Y Z in ACN order (W Y Z X). `shape` says what each channel carries
            of one 300 Hz tone; `tail` is the quiet tone a burst falls to -- above the silence gate,
            so the release is the whole of the fall. Returns `level` as the matrix reads it. */
        const auto levelAfter = [](std::array<float, 4> shape, float releaseMs, bool withTail, int width = 4) {
            BambiReverbProcessor proc;
            const auto first =
                width == 4 ? juce::AudioChannelSet::ambisonic(1) : juce::AudioChannelSet::discreteChannels(width);
            if (!proc.setBusesLayout(layout(first, juce::AudioChannelSet::disabled(), first))) return -1.0f;
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 256);
            setParameter(proc, "level.release", releaseMs);
            proc.prepareToPlay(kRate, 256);
            juce::AudioBuffer<float> buffer(width, 256);
            juce::MidiBuffer midi;
            const auto loud = static_cast<juce::int64>(0.5 * kRate), end = static_cast<juce::int64>(0.75 * kRate);
            for (juce::int64 at = 0; at < (withTail ? end : loud); at += 256) {
                head.time = at;
                buffer.clear();
                for (int i = 0; i < 256; ++i) {
                    const double t = static_cast<double>(at + i) / kRate;
                    const float amp = at + i < loud ? 0.5f : 0.01f;
                    const float x = amp * static_cast<float>(std::sin(6.2831853 * 300.0 * t));
                    for (int c = 0; c < 4; ++c) buffer.setSample(c, i, shape[static_cast<std::size_t>(c)] * x);
                }
                proc.processBlock(buffer, midi);
            }
            return static_cast<bambi::host::TargetHost&>(proc).liveSourceValue(0);
        };

        /*  Two sources in antiphase, left and right: W is nothing and Y is twice one of them. Catches
            the detector going back to W alone, which hears this as silence -- and a ducker then
            lets the wet through under a loud field. */
        const float single = levelAfter({1.0f, 1.0f, 0.0f, 0.0f}, 200.0f, false);
        const float antiphase = levelAfter({0.0f, 2.0f, 0.0f, 0.0f}, 200.0f, false);
        check(single > 0.5f, "one source in the field is heard (" + juce::String(single, 3) + ")");
        check(antiphase > 0.5f, "and so is a pair whose W cancels (" + juce::String(antiphase, 3) + ")");

        /*  A bus wider than its order -- eight channels carry order 1 and four spare. Catches the
            power being taken over the bus's width: root 8 where the order says 2 reads 1.5 dB low,
            and the spare channels are whatever a host left in them. */
        const float wide = levelAfter({1.0f, 1.0f, 0.0f, 0.0f}, 200.0f, false, 8);
        check(std::abs(wide - single) < 0.002f, "a bus wider than its order reads the same level (" +
                                                    juce::String(wide, 3) + " against " + juce::String(single, 3) +
                                                    ")");

        //  Catches the parameter never reaching the detector: both then fall alike.
        const float fast = levelAfter({1.0f, 1.0f, 0.0f, 0.0f}, 30.0f, true);
        const float slow = levelAfter({1.0f, 1.0f, 0.0f, 0.0f}, 1500.0f, true);
        check(fast < 0.35f && slow > 0.6f, "level.release sets how fast it falls (" + juce::String(fast, 3) +
                                               " against " + juce::String(slow, 3) + ")");

        /*  Catches the tile reading it as an amount -- everything in the modulation group was one,
            and 200 ms drew as "20000 %". */
        check(bambi::ui::tileValueText(bambi::reverbParams()[bambi::reverbParams().byKey("level.release")], 200.0) ==
                      "200 ms" &&
                  bambi::ui::tileValueText(bambi::reverbParams()[bambi::reverbParams().byKey("mod.amount.level")],
                                           1.0) == "100 %",
              "a release reads in milliseconds, and an amount still as a percentage");

        //  The control is where the detector's other setting is: on Level's own page, and only there.
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        const auto id = [](const char* k) { return static_cast<bambi::ParamId>(bambi::reverbParams().byKey(k)); };
        if (ed != nullptr) {
            ed->showSource(bambi::sourceSlot(bambi::MatrixTab::Features, 0));
            ed->tick();
            check(ed->drawnSomewhere(id("level.release")) && !ed->drawnSomewhere(id("sc_level.release")),
                  "level's page has its release, and not the sidechain's");
            ed->showSource(bambi::sourceSlot(bambi::MatrixTab::Sidechain, 0));
            ed->tick();
            check(ed->drawnSomewhere(id("sc_level.release")) && !ed->drawnSomewhere(id("level.release")),
                  "the sidechain's level has its own");
            ed->showSource(bambi::sourceSlot(bambi::MatrixTab::Features, 1));
            ed->tick();
            check(!ed->drawnSomewhere(id("level.release")), "and attack's page has neither");
        }
    }

    std::printf("\nwhat a window redraws for, when the change is not its own\n");
    {
        /*  A local edit repaints through `notify()`, so these show only when another window made
            the change, or a state load did. Catches the comparison reading a trigger's input alone,
            and the render quality compared by nobody. */
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        bambi::ui::ReverbControlState controls(proc);
        controls.refresh();
        check(!controls.refresh(), "nothing changed, and nothing is reported");
        proc.document().edit("gate", [](bambi::PluginState& s) { s.envTriggers[0].gate = bambi::TriggerGate::Held; });
        check(controls.refresh(), "an envelope's gate changed elsewhere is a change");
        proc.document().edit("quality", [](bambi::PluginState& s) { s.renderQuality = bambi::RenderQuality::Same; });
        check(controls.refresh(), "and so is the render quality");
        check(!controls.refresh(), "and then nothing again");
        //  The comparison is every plugin's (PatchControls); a moved node is the case a count missed.
        proc.document().edit("nodes", [](bambi::PluginState& s) { s.trajectory.nodes.assign(2, bambi::Node{}); });
        controls.refresh();
        proc.document().edit("move", [](bambi::PluginState& s) { s.trajectory.nodes[1].p = {0.0, 1.0, 0.0}; });
        check(controls.refresh(), "a node moved elsewhere is a change, though the count is the same");
    }

    std::printf("\nthe settings page\n");
    {
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed == nullptr) {
            check(false, "no editor: the settings checks cannot run");
            return 1;
        }
        check(!ed->settingsOpen(), "the page is closed until it is asked for");
        ed->openSettings(true);
        check(ed->settingsOpen() && ed->footerIsInFrontOfSettings(),
              "open, it covers the scene and the tabs and leaves the footer in front of it");

        /*  The name is identity: it goes to the processor, comes back in the saved state, and an
            empty one gives the instance back to the track's name. Catches the field not reaching
            the identity, the state not carrying it, and an emptied field keeping the old name. */
        ed->typeInstanceName("  plate, long  ");
        check(proc.identity().label == "plate, long", "a typed name is the instance's, trimmed");
        {
            juce::MemoryBlock saved;
            proc.getStateInformation(saved);
            BambiReverbProcessor reopened;
            reopened.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
            check(reopened.identity().label == "plate, long", "and a saved project opens with it");
        }
        ed->typeInstanceName("a name far too long for the thirty-two bytes a label has on the bus");
        check(proc.identity().label.size() <= 31, "a name is held to what the bus's label can carry");
        ed->typeInstanceName("");
        check(proc.identity().label.empty(), "and an emptied field gives the name back to the track");

        /*  When rendering is state. Catches the segments not reaching the processor. */
        check(proc.renderQuality() == bambi::RenderQuality::Realistic, "a render opens at realistic");
        check(ed->clickSettingsOption(1, 0) && proc.renderQuality() == bambi::RenderQuality::Same,
              "and the page's segment sets what a bounce does");
        /*  While playing is a host parameter, set through the controls. Catches the row drawn and
            not wired, and the footer keeping a switch it no longer has. */
        {
            auto* playing =
                proc.hostParameter(static_cast<bambi::ParamId>(bambi::reverbParams().byKey("quality.playing")));
            check(ed->clickSettingsOption(0, 0) && playing != nullptr && playing->getValue() < 0.5f,
                  "and the row above it sets the quality while playing");
            if (playing != nullptr) playing->setValueNotifyingHost(1.0f);
            check(ed->footer()
                      .switchArea(static_cast<bambi::ParamId>(bambi::reverbParams().byKey("quality.playing")))
                      .isEmpty(),
                  "and the footer no longer carries it");
        }

        /*  The open page follows what changes under it -- an undo, another window, the session's
            peers -- none of which is a press on the page. Catches the frame not asking: the page
            would go on showing the choice as it was. */
        ed->tick();
        const int settled = ed->settingsRefreshes();
        ed->tick();
        check(ed->settingsRefreshes() == settled, "an open page with nothing changed under it asks for no repaint");
        proc.setRenderQuality(bambi::RenderQuality::Realistic);  // as an undo would
        ed->tick();
        check(ed->settingsRefreshes() == settled + 1, "and one whose choice moved under it repaints, once");

        check(ed->closeSettingsByItsCross() && !ed->settingsOpen(), "the cross closes it");
    }

    std::printf("\na region is copied and pasted through the clipboard\n");
    {
        /*  Through the buttons, on a clipboard of the check's own -- the person running this has
            something on theirs. The send is copied and pasted onto the return, which is the case a
            key-for-key copy cannot do: `region1.yaw` has to arrive as `region2.yaw`. */
        juce::String held;
        const auto systems = bambi::ui::textClipboard();
        bambi::ui::textClipboard() = {[&held] { return held; }, [&held](const juce::String& t) { held = t; }};
        bambi::ui::clipboardChanged();

        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed == nullptr) {
            check(false, "no editor: the clipboard checks cannot run");
            return 1;
        }
        const auto& m = bambi::reverbParams();
        const auto id = [&m](const char* key) { return static_cast<bambi::ParamId>(m.byKey(key)); };
        const auto set = [&](const char* key, float value) {
            if (auto* p = proc.hostParameter(id(key))) p->setValueNotifyingHost(p->convertTo0to1(value));
        };
        const auto live = [&](const char* key) {
            auto* p = proc.hostParameter(id(key));
            return p == nullptr ? -999.0f : p->convertFrom0to1(p->getValue());
        };

        ed->showTab(1);
        ed->showSlot(0);
        ed->chooseRegionKind(0, static_cast<int>(bambi::RegionKind::Band));
        set("region1.yaw", 40.0f);
        set("region1.thickness", 25.0f);
        set("region1.side", 1.0f);        // the use's: it must not travel
        set("mod.amount.region1", 0.5f);  // and nor must this
        set("lfo1.rate", 0.75f);
        proc.document().edit("a row", [&](bambi::PluginState& s) {
            bambi::setCellDepth(bambi::reverbMod(), s, bambi::MatrixTab::Generators, 0, id("region1.yaw"), 0.5);
        });
        ed->tick();

        check(ed->namedDrawn("region paste") && !ed->clickNamed("region paste"),
              "with no region on the clipboard, paste is drawn and takes no press");
        check(ed->clickNamed("region copy") && held.contains("bambi.region"),
              "copy puts a region descriptor on the clipboard");

        ed->showSlot(1);
        ed->tick();
        check(ed->clickNamed("region paste"), "and on the return's tab, paste takes the press");
        ed->tick();
        const auto after = proc.document().editing();
        check(after.regions[1].shape.kind == bambi::RegionKind::Band, "the return is a band now");
        check(std::abs(live("region2.yaw") - 40.0f) < 0.01f && std::abs(live("region2.thickness") - 25.0f) < 0.01f,
              "with the send's settings, under the return's own keys");
        check(live("region2.side") < 0.5f && std::abs(live("mod.amount.region2") - 1.0f) < 0.01f,
              "and its own side and amount: they are the use's, and did not travel");
        check(std::abs(bambi::cellDepth(after, bambi::MatrixTab::Generators, 0, id("region2.yaw")) - 0.5) < 1e-9,
              "the row from the restart-mode lfo came with it, onto the return's yaw");

        proc.document().undo();
        ed->tick();
        check(proc.document().editing().regions[1].shape.kind != bambi::RegionKind::Band,
              "and one undo takes the shape and the rows back: a paste is one edit");

        /*  A region copied somewhere else lights paste here within a frame or two: the clipboard
            is not the patch, so no refresh reports it. Painted once, by the frame -- `clickNamed`
            paints first and would hide a button that only a repaint by luck brought up to date.
            Catches the frame not looking. */
        {
            const juce::String region = held;
            held = "not a region";
            bambi::ui::clipboardChanged();
            ed->paintNow();
            const int before = ed->panelRepaintsForClipboard();
            held = region;
            bambi::ui::clipboardChanged();
            ed->tick();
            check(ed->panelRepaintsForClipboard() > before,
                  "a region that reaches the clipboard from elsewhere repaints the page that shows paste");
        }

        //  Catches a paste that guesses: whatever is not a current descriptor greys the button.
        held = "{\"bambi.region\":2,\"kind\":\"band\",\"fields\":{}}";
        bambi::ui::clipboardChanged();
        check(ed->namedDrawn("region paste") && !ed->clickNamed("region paste"),
              "a descriptor of another version greys paste again: it fails closed");

        bambi::ui::textClipboard() = systems;
        bambi::ui::clipboardChanged();
    }

    bambi::editor::checkEffectRetrigger(spec);

    std::printf("\na region angle set back to zero takes its turn with it\n");
    {
        BambiReverbProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiReverbEditor*>(editor.get());
        if (ed == nullptr) {
            check(false, "no editor: the turn checks cannot run");
            return 1;
        }
        for (int slot = 0; slot < 2; ++slot) ed->chooseRegionKind(slot, static_cast<int>(bambi::RegionKind::Spot));

        const auto id = [](const char* key) { return static_cast<bambi::ParamId>(bambi::reverbParams().byKey(key)); };
        const auto set = [&](const char* key, float value) {
            if (auto* p = proc.hostParameter(id(key))) p->setValueNotifyingHost(p->convertTo0to1(value));
        };
        //  both slots turning, which is what tells a slot mix-up from a working one
        set("region1.yaw_rate", 60.0f);
        set("region2.yaw_rate", 60.0f);

        //  playing, and moving on: set to restart, a rate turns only while the transport runs
        PlayingHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(34, 128);
        juce::MidiBuffer midi;
        const auto run = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                buffer.clear();
                proc.processBlock(buffer, midi);
                head.time += buffer.getNumSamples();
            }
            ed->tick();
        };
        const auto turnOf = [&](int slot) {
            return static_cast<double>(
                proc.diagnostics().regionTurn[static_cast<std::size_t>(slot * 3)].load(std::memory_order_relaxed));
        };
        run(200);
        check(turnOf(0) > 0.3 && turnOf(1) > 0.3, "both slots' yaw rates have turned them");

        //  the send's tile, on the regions tab with the send selected
        ed->showTab(1);
        ed->showSlot(0);
        ed->setRegionTab(1);
        ed->tick();
        ed->doubleClickTile(id("region1.yaw"));
        run(2);
        check(turnOf(0) < 0.05, "a double-click on the send's yaw clears the SEND's turn");
        /*  Catches every way the two slots can be crossed: one atomic for both, the wrong index
            into the mask array, or a control step that hands slot 0's mask to both clocks. */
        check(turnOf(1) > 0.3, "and leaves the RETURN's alone");

        ed->showSlot(1);
        ed->tick();
        ed->doubleClickTile(id("region2.yaw"));
        run(2);
        check(turnOf(1) < 0.05, "and the return's own tile clears the return's");
    }

    //  ---- presets: the shared processor's and the shared window's, so the checks are shared too
    std::printf("\npresets\n");
    std::printf("\na room chosen is switched to whole, not glided into\n");
    {
        /*  A cathedral, then the ambience button, against the ambience from the start: after the
            switch the output is no louder than the room it switched to. Gliding the size (250 ms)
            behind the decay (40 ms) would pass through large rooms with short decays, whose dry is
            up to 27 dB above the input. Catches the settle on a room change dropped. */
        const auto choose = [](BambiReverbProcessor& proc, bambi::ReverbPreset which) {
            bambi::RoomState room;
            const auto& m = bambi::reverbParams();
            bambi::applyPreset(
                m, which,
                [&proc](int at, float normalised) {
                    if (auto* p = proc.hostParameter(static_cast<bambi::ParamId>(at)))
                        p->setValueNotifyingHost(normalised);
                },
                room);
            proc.document().edit("room", [room](bambi::PluginState& st) { st.room = room; });
        };
        const auto loudestFrom = [](BambiReverbProcessor& proc, PlayingHead& head, juce::int64 from, juce::int64 to,
                                    juce::int64 measureFrom) {
            const int width = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            const int ins = proc.getMainBusNumInputChannels();
            juce::AudioBuffer<float> buffer(width, 128);
            juce::MidiBuffer midi;
            double loudest = 0.0;
            for (juce::int64 at = from; at < to; at += 128) {
                head.time = at;
                buffer.clear();
                for (int c = 0; c < ins; ++c)
                    for (int i = 0; i < 128; ++i) buffer.setSample(c, i, fieldSample(c, at + i));
                proc.processBlock(buffer, midi);
                if (at >= measureFrom) {
                    double e = 0.0;
                    for (int c = 0; c < proc.getMainBusNumOutputChannels(); ++c)
                        for (int i = 0; i < 128; ++i)
                            e += static_cast<double>(buffer.getSample(c, i)) * buffer.getSample(c, i);
                    loudest = std::max(loudest, e / 128.0);
                }
            }
            return 10.0 * std::log10(loudest + 1e-30);
        };
        const auto at = [](double seconds) { return static_cast<juce::int64>(seconds * kRate); };

        BambiReverbProcessor switched, steady;
        PlayingHead headA, headB;
        switched.setPlayHead(&headA);
        steady.setPlayHead(&headB);
        switched.prepareToPlay(kRate, 128);
        steady.prepareToPlay(kRate, 128);
        choose(switched, bambi::ReverbPreset::Cathedral);
        choose(steady, bambi::ReverbPreset::Ambience);
        loudestFrom(switched, headA, 0, at(1.5), at(1.5));
        choose(switched, bambi::ReverbPreset::Ambience);
        const double after = loudestFrom(switched, headA, at(1.5), at(2.3), at(1.5));
        const double room = loudestFrom(steady, headB, 0, at(2.3), at(1.5));
        check(after <= room + 1.0, "after the ambience button, no louder than the ambience itself (" +
                                       juce::String(after, 1) + " against " + juce::String(room, 1) + " dB)");
    }

    std::printf("\nthe region in the scene: the shared checks\n");
    bambi::editor::checkRegions<BambiReverbProcessor>(
        {[](bambi::editor::PluginEditor& e, bool open) {
             if (auto* reverb = dynamic_cast<BambiReverbEditor*>(&e)) reverb->showTab(open ? 1 : 0);  // the regions tab
         },
         [](bambi::editor::PluginEditor& e) -> std::optional<bambi::Vec3> {
             if (auto* reverb = dynamic_cast<BambiReverbEditor*>(&e)) return reverb->probeDirection();
             return std::nullopt;
         }},
        [](bool ok, const juce::String& what) { check(ok, what); });

    bambi::editor::checkPresets<BambiReverbProcessor>({"room.decay"},
                                                      [](bool ok, const juce::String& what) { check(ok, what); });

    return checkVerdict();
}
