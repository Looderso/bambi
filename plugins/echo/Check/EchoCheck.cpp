// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-echo-check — Echo's host-contract properties, on the shipped sources, with no host.
//
//  The encoder's bambi-plugin-check proves these for a plugin that makes a field. Echo is the
//  first that takes one and returns it, so the same questions have different answers:
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

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/Strip.h"
#include "bambi/echo/params.hpp"
#include "bambi/editor/CheckKit.h"
#include "bambi/editor/EffectChecks.h"
#include "bambi/editor/PresetChecks.h"
#include "bambi/editor/RegionChecks.h"
#include "bambi/host/BlockBench.h"
#include "bambi/host/LinkNode.h"
#include "bambi/host/TestPlayHead.h"
#include "bambi/math/sh.hpp"
#include "bambi/mod/matrix.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/scene/energy.hpp"
#include "bambi/ui/ParameterPage.h"
#include "bambi/ui/Widgets.h"

//  Catches: a dB level's bar that shrinks as the level rises instead of growing.
#define CHECK_DB_FILL(d)                                                                              \
    do {                                                                                              \
        const double lowFill =                                                                        \
            bambi::ui::normalised(bambi::echoParams(), static_cast<bambi::ParamId>((d).id), (d).min); \
        const double highFill =                                                                       \
            bambi::ui::normalised(bambi::echoParams(), static_cast<bambi::ParamId>((d).id), (d).max); \
        check(highFill > lowFill, "a dB level's bar grows with the level");                           \
    } while (false)

//  A check that cannot go on without an editor says so once, rather than crashing on a null.
#define REQUIRE_EDITOR(p)                                                \
    do {                                                                 \
        if ((p) == nullptr) {                                            \
            check(false, "no editor: the checks after this cannot run"); \
            return checkVerdict();                                       \
        }                                                                \
    } while (false)

namespace {
constexpr double kRate = bambi::editor::kCheckRate;
using Processor = BambiEchoProcessor;

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
        setParameter(p, "tap1.ms", 120.0f);
        setParameter(p, "tap1.feedback", -6.0f);
        setParameter(p, "tap1.spin", 40.0f);
        setParameter(p, "tap1.synced", 0.0f);
        setParameter(p, "region1.yaw_rate", 120.0f);
    };
    return bambi::editor::renderField<Processor>(l, block, seconds, setUp, {state, locateAt, locateTo, startAt, patch});
}

bambi::editor::EffectCheckSpec<Processor> effectSpec() {
    bambi::editor::EffectCheckSpec<Processor> spec;
    spec.tool = "bambi-echo-check";
    spec.whatIsInIt = "the echo";
    spec.params = &bambi::echoParams();
    spec.foreignProduct = bambi::Product::Reverb;
    spec.render = [](const Layout& l, int block, double seconds, const bambi::editor::RenderRun& run) {
        return render(l, block, seconds, run.state, run.locateAt, run.locateTo, run.startAt, run.patch);
    };
    spec.stateKey = "tap2.ms";
    spec.stateValue = 313.0f;
    spec.stateTolerance = 0.5f;
    spec.stateSetUp = [](Processor& p) { setParameter(p, "tap2.on", 1.0f); };
    spec.drivenKey = "tap1.spin";
    spec.drivenWhat = "a tap's spin";
    spec.drivenRegions = [](bambi::PluginState& st) { st.regions[0].shape.kind = bambi::RegionKind::Spot; };
    spec.processChannels = 18;
    spec.product = bambi::Product::Echo;
    spec.linkDirectory = "/bambi.ec.";
    spec.plural = "Echoes";
    spec.withArticle = "an Echo";
    spec.remoteKey = "tap1.level";
    spec.remoteValue = -12.0f;
    return spec;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;

    /*  `--bench`: what a whole processBlock costs, as a host drives it. The engine alone is
        `bambi-bench`'s; this is the plugin round it. Release only -- a Debug number means nothing. */
    const auto spec = effectSpec();
    if (argc > 1 && juce::String(argv[1]) == "--bench") return bambi::editor::benchEffect(spec);

    std::printf("bambi-echo-check\n\n");

    bambi::editor::checkEffectLayouts<Processor>();
    bambi::editor::checkEffectRenders(spec);
    bambi::editor::checkEffectLocates(spec);

    bambi::editor::checkEffectState(spec);
    bambi::editor::checkEffectHandoff(spec);

    bambi::editor::checkEffectLinkBus(spec);

    std::printf("\nthe shared parameter page\n");
    {
        /*  The proof that `ui::ParameterPage` is shared and not merely extracted: a plugin with a
            different manifest, no scene and no editor draws a page from it and it works. An
            extraction with one user is a guess, and this is the second user. */
        BambiEchoProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);

        /*  What a page asks of a plugin, and no more: no tabs, no source columns, no other instance
            -- those are the matrix's, and are `MatrixModel`. A stub is where behaviour drifts, so
            the split leaves this page with none. */
        struct Model final : bambi::ui::PatchModel {
            BambiEchoProcessor& p;
            bambi::PluginState patch;
            bambi::ParamId provisional{bambi::kNoParamId};
            int notified{0};
            explicit Model(BambiEchoProcessor& proc) : p(proc), patch(proc.document().editing()) {}

            const bambi::ParamManifest& manifest() const override { return bambi::echoParams(); }
            const bambi::ModManifest& modManifest() const override { return bambi::echoMod(); }
            const bambi::PluginState& state() const override { return patch; }
            bambi::ParamId provisionalRow() const override { return provisional; }
            void setProvisionalRow(bambi::ParamId at) override { provisional = at; }
            void beginParameter(bambi::ParamId at) override {
                if (auto* q = p.hostParameter(at)) q->beginChangeGesture();
            }
            void setParameter(bambi::ParamId at, float v) override {
                if (auto* q = p.hostParameter(at)) q->setValueNotifyingHost(v);
            }
            void endParameter(bambi::ParamId at) override {
                if (auto* q = p.hostParameter(at)) q->endChangeGesture();
            }
            void applyEdit(std::string_view name, const bambi::UndoStack::Edit& c) override {
                p.document().edit(name, c);
            }
            void applyEditDrag(std::string_view name, std::string_view key, const bambi::UndoStack::Edit& c) override {
                p.document().editCoalescing(name, key, c);
            }
            void finishDrag() override { p.document().endGesture(); }
            void notifyChanged() override {
                ++notified;
                patch = p.document().editing();
            }
            //  a page draws a source's live value too; this stub has no engine behind it
            double sourceValueOf(int) override { return 0.0; }
        };

        //  A page of Echo's own: which parameters, in what order, under what titles. All a page says.
        struct Page final : bambi::ui::ParameterPage {
            using bambi::ui::ParameterPage::ParameterPage;
            void paint(juce::Graphics& g) override {
                liveRegion() = {};
                clearNameAreas();
                const auto& m = model().manifest();
                const auto id = [&m](const char* k) { return static_cast<bambi::ParamId>(m.byKey(k)); };
                auto content = getLocalBounds().toFloat().reduced(8.0f);
                paintGroup(g, content, content.getY(), "level",
                           {id("tap1.level"), id("tap1.feedback"), id("tap1.spin"), id("tap1.skew")});
            }
        };

        Model model(proc);
        Page page(model);
        page.setSize(320, 400);

        juce::Image image(juce::Image::ARGB, 320, 400, true);
        {
            juce::Graphics g(image);
            page.paint(g);
        }
        check(true, "a page of Echo's parameters paints without a scene, an editor or the encoder");

        const auto level = static_cast<bambi::ParamId>(bambi::echoParams().byKey("tap1.level"));
        const auto box = page.nameArea(level);
        check(!box.isEmpty(), "and the tile registered where it was drawn");

        if (!box.isEmpty()) {
            //  A drag on the tile must reach this plugin's host parameter, through the manifest alone.
            auto* param = proc.hostParameter(level);
            const float before = param == nullptr ? 0.0f : param->getValue();
            const auto centre = box.getCentre();
            const juce::MouseEvent down(juce::Desktop::getInstance().getMainMouseSource(), centre,
                                        juce::ModifierKeys::noModifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &page, &page,
                                        juce::Time::getCurrentTime(), centre, juce::Time::getCurrentTime(), 1, false);
            page.mouseDown(down);
            const auto up = centre.translated(0.0f, -60.0f);
            const juce::MouseEvent dragged(juce::Desktop::getInstance().getMainMouseSource(), up,
                                           juce::ModifierKeys::noModifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &page, &page,
                                           juce::Time::getCurrentTime(), centre, juce::Time::getCurrentTime(), 1,
                                           false);
            page.mouseDrag(dragged);
            page.mouseUp(dragged);
            check(param != nullptr && param->getValue() > before,
                  "a drag on it moves Echo's own host parameter, through the manifest alone");
        }

        /*  A greyed tile is inert: it takes no drag and no click. Asserted positively -- a
            tile drawn disabled registers nothing, so the same drag that moved the one above moves
            nothing here. Without this, "greyed" is only a colour. */
        struct GreyPage final : bambi::ui::ParameterPage {
            using bambi::ui::ParameterPage::ParameterPage;
            bool enabled{true};
            void paint(juce::Graphics& g) override {
                clearRegions();
                clearNameAreas();
                const auto id = static_cast<bambi::ParamId>(model().manifest().byKey("tap1.level"));
                paintGroup(g, getLocalBounds().toFloat().reduced(8.0f), getLocalBounds().toFloat().reduced(8.0f).getY(),
                           {}, {Cell{[this, id](juce::Graphics& gg, juce::Rectangle<float> tile) {
                               addParameterTile(gg, tile, id, {}, enabled);
                           }}});
            }
        };
        GreyPage grey(model);
        grey.setSize(320, 120);
        juce::Image greyImage(juce::Image::ARGB, 320, 120, true);
        /*  Painted enabled first, only to learn where the tile is: a disabled tile records no area,
            which is the very thing being asserted. Then painted greyed and dragged at that spot. */
        {
            juce::Graphics g(greyImage);
            grey.paint(g);
        }
        const auto greyBox = grey.nameArea(level);
        grey.enabled = false;
        {
            juce::Graphics g(greyImage);
            grey.paint(g);
        }
        check(!greyBox.isEmpty() && grey.nameArea(level).isEmpty(),
              "a greyed tile records no area at all, where the same tile enabled does");
        auto* param = proc.hostParameter(level);
        const float held = param == nullptr ? 0.0f : param->getValue();
        const auto at = greyBox.getCentre();
        /*  Downward: the drag above left this parameter at the top of its range, and dragging up
            from there moves nothing whether the tile is live or not. */
        const juce::MouseEvent press(juce::Desktop::getInstance().getMainMouseSource(), at,
                                     juce::ModifierKeys::noModifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &grey, &grey,
                                     juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false);
        grey.mouseDown(press);
        const auto moved = at.translated(0.0f, 60.0f);
        const juce::MouseEvent slide(juce::Desktop::getInstance().getMainMouseSource(), moved,
                                     juce::ModifierKeys::noModifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &grey, &grey,
                                     juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false);
        grey.mouseDrag(slide);
        grey.mouseUp(slide);
        check(param != nullptr && juce::exactlyEqual(param->getValue(), held),
              "and a greyed tile is inert: the same drag on one moves nothing");
    }

    std::printf("\nthe editor\n");
    {
        BambiEchoProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* echo = dynamic_cast<BambiEchoEditor*>(editor.get());
        check(echo != nullptr, "Echo has an editor, and it opens");
        if (echo == nullptr) {
            return checkVerdict();
        }

        /*  Every tap's every control is reached somewhere: on one of the three categories, or in
            the selector row. "Reached" means drawn with a box a click can land on -- not merely
            present in the manifest -- so a category dropped from the panel fails this, and so does
            a parameter nobody thought to place. */
        const auto& m = bambi::echoParams();
        int reached = 0, missing = 0;
        juce::String firstMissing;
        for (int tap = 0; tap < bambi::kEchoTaps; ++tap) {
            echo->selectTap(tap);
            for (int i = 0; i < m.size(); ++i) {
                const auto key = std::string(m[i].key);
                const auto prefix = "tap" + std::to_string(tap + 1) + ".";
                if (!key.starts_with(prefix)) continue;
                const auto id = static_cast<bambi::ParamId>(i);
                bool found = echo->drawnSomewhere(id);
                for (int category = 0; category < 3 && !found; ++category) {
                    echo->showCategory(category);
                    found = echo->drawnSomewhere(id);
                }
                if (found)
                    ++reached;
                else {
                    ++missing;
                    if (firstMissing.isEmpty()) firstMissing = juce::String(key);
                }
            }
        }
        check(missing == 0, "every control of every tap is reachable (" + juce::String(reached) + " of " +
                                juce::String(reached + missing) +
                                (missing == 0 ? juce::String() : ", first missing " + firstMissing) + ")");

        /*  A level in decibels grows its bar as the level rises. Both of Echo's output levels run
            -60..+12, so a rule that fills a range crossing zero from zero made the bar shrink as
            the level went up -- which reads as a control that does not work. */
        {
            const auto wet = static_cast<bambi::ParamId>(m.byKey("output.wet"));
            const auto& d = bambi::echoParams()[static_cast<int>(wet)];
            CHECK_DB_FILL(d);
        }

        /*  Stated as two rules and checked as two: "Switching tab never moves the selection
            and selecting never moves the tab." The row says which tap, the tab says what. */
        echo->selectTap(2);
        echo->showCategory(1);
        echo->showCategory(2);
        check(echo->selectedTap() == 2, "switching category never moves the selected tap");
        echo->selectTap(0);
        check(echo->shownCategory() == 2, "and selecting a tap never moves the category");

        //  A click on the row selects it, the way the mouse does -- not by setting state.
        check(echo->clickTapRow(3) && echo->selectedTap() == 3, "a click on a tap's row selects it");

        /*  The on/off must not select: silencing a tap cannot drag its panel open. This is
            the registration order -- the row goes down first and the switch on top of it, because
            HitArea hit-tests back to front. Clicked through the real region rather than by asking
            whether a name area exists, since a switch has no name area of its own. */
        echo->selectTap(1);
        const auto onOff = static_cast<bambi::ParamId>(m.byKey("tap4.on"));
        auto* onOffParam = proc.hostParameter(onOff);
        const float wasOn = onOffParam == nullptr ? 0.0f : onOffParam->getValue();
        check(echo->clickTapSwitch(3), "a tap's on/off switch is clickable");
        check(onOffParam != nullptr && !juce::exactlyEqual(onOffParam->getValue(), wasOn),
              "and clicking it toggles the tap");
        check(echo->selectedTap() == 1, "and silencing a tap does not select it: the row keeps its selection");

        //  The three tabs are Echo's own names, and each draws something.
        for (int tab = 0; tab < 3; ++tab) {
            echo->showTab(tab);
            const juce::Image shot = echo->createComponentSnapshot(echo->getLocalBounds(), true, 1.0f);
            check(shot.isValid() && shot.getWidth() > 0, "tab " + juce::String(tab) + " paints");
        }
        echo->showTab(0);

        /*  The matrix draws Echo's targets, not the encoder's: its base row is Echo's own. */
        bambi::PluginState patch = proc.document().editing();
        const auto rows = bambi::matrixTargets(bambi::echoMod(), patch);
        bool echoesOwn = !rows.empty();
        for (const auto id : rows) echoesOwn = echoesOwn && static_cast<int>(id) < m.size();
        check(echoesOwn,
              "the matrix's rows are Echo's own targets (" + juce::String(static_cast<int>(rows.size())) + ")");
    }

    std::printf("\nthe window draws the taps as they play\n");
    {
        /*  An LFO on tap 1's feedback: the engine publishes every value as it has it, and the window's patch
            as played -- what the strip and the ping are drawn from -- carries the moving feedback, not the set
            one. Catches the published values not reaching the window. */
        BambiEchoProcessor proc;
        PlayingHead head;
        proc.setPlayHead(&head);
        proc.prepareToPlay(kRate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        const auto& m = bambi::echoParams();
        const auto feedback = static_cast<bambi::ParamId>(m.byKey("tap1.feedback"));
        setParameter(proc, "tap1.feedback", -12.0f);
        setParameter(proc, "lfo1.sync", 0.0f);
        setParameter(proc, "lfo1.rate", 2.0f);
        proc.document().edit("feedback row", [feedback](bambi::PluginState& st) {
            st.matrix.push_back({bambi::MatrixTab::Generators, 0, feedback, 0.3});
        });
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()),
                                        512);
        juce::MidiBuffer midi;
        double furthest = 0.0;
        for (int b = 0; b < 60; ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
            head.time += 512;
            proc.linkTick();
            if (b % 4 == 3) {
                ed->tick();
                const auto played = ed->patchAsPlayed();
                furthest = std::max(
                    furthest, std::abs(static_cast<double>(played.params[static_cast<std::size_t>(feedback)]) + 12.0));
            }
        }
        check(furthest > 1.0, "the patch the strip is drawn from carries the feedback as the engine moves it (" +
                                  juce::String(furthest, 1) + " dB from the set -12)");
        /*  And the window repaints for it, with no paint forced here: a frame alone must ask for it. What a
            tile's live mark or the strip shows is only seen if something repaints it, and a check that paints
            first hides exactly that. Catches: the frame not comparing what the engine published. */
        ed->paintNow();  // a window that has painted once, as every open one has: it knows where its tiles and strip are
        ed->tick();
        const int idle = ed->liveRepaintsAsked();
        ed->tick();  // nothing played between: nothing moved
        const bool quiet = ed->liveRepaintsAsked() == idle;
        const int before = ed->liveRepaintsAsked();
        for (int b = 0; b < 8; ++b) {
            buffer.clear();
            proc.processBlock(buffer, midi);
            head.time += 512;
            proc.linkTick();
        }
        ed->tick();
        check(quiet && ed->liveRepaintsAsked() > before,
              "the window asks to repaint when the engine moves a value, and not when nothing moved");
    }

    std::printf("\nthe strip\n");
    {
        /*  What the strip shows, checked without painting it. A picture whose only assertion is
            "it did not crash" is a picture nobody has checked. */
        const auto& m = bambi::echoParams();
        bambi::PluginState patch{m};
        patch.resetParamsToDefaults(m);
        const auto set = [&](const char* key, float v) { patch.params[static_cast<std::size_t>(m.byKey(key))] = v; };

        //  one tap, free, a tenth of a second, no feedback to speak of
        set("tap1.on", 1.0f);
        set("tap1.synced", 0.0f);
        set("tap1.ms", 100.0f);
        set("tap1.offset_ms", 0.0f);
        set("tap1.feedback", -12.0f);
        auto content = bambi::ui::stripContent(patch, 120.0, kRate, 3);
        check(content.taps[0].on && !content.taps[0].passes.empty(),
              "an enabled tap has passes (" + juce::String(static_cast<int>(content.taps[0].passes.size())) + ")");
        check(std::abs(content.taps[0].periodSeconds - 0.1) < 0.002,
              "and its period is what the control step resolved, not a second reading of the keys");
        check(std::abs(content.taps[0].passes[0].seconds - 0.1) < 0.002, "the first pass lands one period in");

        bool falling = true;
        for (std::size_t i = 1; i < content.taps[0].passes.size(); ++i)
            falling = falling && content.taps[0].passes[i].gain < content.taps[0].passes[i - 1].gain;
        check(falling, "each pass is quieter than the one before it");

        /*  The stop is three stops, and each is asserted by the count it produces, not by a
            bound the others already satisfy. */
        const auto countWith = [&](float ms, float feedbackDb) {
            set("tap1.ms", ms);
            set("tap1.feedback", feedbackDb);
            return bambi::ui::stripContent(patch, 120.0, kRate, 3).taps[0].passes.size();
        };
        //  level -3, feedback -12: -15, -27, -39, -51. The fourth is the last above -48.
        check(countWith(100.0f, -12.0f) == 4, "the level floor stops the walk at 48 dB down (4 passes)");
        //  900 ms a pass, feedback shallow: 0.9, 1.8, 2.7, 3.6 -- the fifth is past four seconds.
        check(countWith(900.0f, -0.3f) == 4, "four seconds stops it too (4 passes at 900 ms)");
        //  20 ms and barely any fall: neither of those bites, so the 32-pass cap is what does.
        check(countWith(20.0f, -0.3f) == 32, "and failing both, the walk caps at 32 passes");
        set("tap1.ms", 100.0f);
        set("tap1.feedback", -12.0f);

        /*  The span follows the longest enabled tap, rounded up to a whole beat. Checked at a span
            that is not already a whole beat: 300 ms a pass, four passes, is 1.2 s, which at 120 bpm
            rounds up to 1.5. A span that happened to be a beat already would pass without rounding. */
        set("tap1.ms", 300.0f);
        const auto rounded = bambi::ui::stripContent(patch, 120.0, kRate, 3);
        check(rounded.taps[0].passes.size() == 4 && std::abs(rounded.taps[0].passes.back().seconds - 1.2) < 0.01,
              "the longest pass lands at 1.2 s");
        check(
            std::abs(rounded.spanSeconds - 1.5) < 1e-6,
            "and the span rounds it up to a whole beat: 1.5 s, not 1.2 (" + juce::String(rounded.spanSeconds, 3) + ")");
        set("tap1.ms", 100.0f);
        content = bambi::ui::stripContent(patch, 120.0, kRate, 3);

        set("tap2.on", 1.0f);
        set("tap2.synced", 0.0f);
        set("tap2.ms", 900.0f);
        set("tap2.feedback", -3.0f);
        const auto longer = bambi::ui::stripContent(patch, 120.0, kRate, 3);
        check(longer.spanSeconds > content.spanSeconds, "a longer tap lengthens the shared axis (" +
                                                            juce::String(content.spanSeconds, 2) + " -> " +
                                                            juce::String(longer.spanSeconds, 2) + " s)");
        set("tap2.on", 0.0f);
        const auto back = bambi::ui::stripContent(patch, 120.0, kRate, 3);
        check(std::abs(back.spanSeconds - content.spanSeconds) < 1e-9,
              "and switching it off brings the axis back: the span follows what is ENABLED");

        /*  The band mirrors the plugin's own filter, so the picture's vertical axis says what the
            audio does. Both ends are one-poles, 6 dB an octave -- at 19 kHz a 14 kHz cut is barely
            2.6 dB down, and asserting anything steeper would be asserting a filter this plugin does
            not have. What is asserted is that each cut moves its own end and the middle passes. */
        const auto band = [](double hz, double lo, double hi) { return bambi::ui::bandMagnitude(hz, lo, hi, kRate); };
        const double mid = band(1000.0, 80.0, 14000.0);
        const double low = band(25.0, 80.0, 14000.0);
        check(mid > 0.9 && low < 0.5 * mid, "the band passes its middle and rejects below its low cut (" +
                                                juce::String(low, 3) + " / " + juce::String(mid, 3) + ")");
        check(band(19000.0, 80.0, 14000.0) < band(19000.0, 80.0, 20000.0),
              "and closing the high cut takes the top down with it");
        check(band(100.0, 300.0, 14000.0) < band(100.0, 80.0, 14000.0), "as raising the low cut takes the bottom");
    }

    std::printf("\nthe ping\n");
    {
        BambiEchoProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* echo = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(echo);

        /*  Until it has been placed it sits at the front, and says so: a dashed ring rather than a
            filled dot, so it reads as never placed instead of as a choice. */
        check(!echo->probePlaced(), "the ping starts unplaced");
        check(echo->probeDirection().x > 0.99, "and at the front, where it says nothing about the source");

        //  A click places it. The centre of the globe is the direction facing the viewer.
        check(echo->placeProbe(0.0, 0.0), "a click on the globe places it");
        check(echo->probePlaced(), "and it is placed from then on");

        //  Off the sphere places nothing: a click outside the globe is not a direction.
        const auto before = echo->probeDirection();
        check(!echo->placeProbe(1.6, 1.6), "a click off the globe places nothing");
        check(echo->probeDirection().x == before.x && echo->probeDirection().z == before.z,
              "and leaves it where it was");

        //  The equirect is editable too, and is where elevation is edited.
        check(echo->placeProbe(0.0, 0.5, false), "a click on the equirect places it as well");
        check(echo->probeDirection().z > 0.1, "and the equirect's upper half is up");

        /*  A click places it; a drag never does -- a drag on the globe orbits it. Driven
            through the real mouse path, because `placePing` above sets it directly and so cannot see
            the difference. */
        echo->clickProbe(0.0, 0.0);
        const auto clicked = echo->probeDirection();
        echo->dragProbe(0.3, 0.0, -0.3, 0.1);
        const auto afterDrag = echo->probeDirection();
        check(std::abs(afterDrag.x - clicked.x) < 1e-9 && std::abs(afterDrag.y - clicked.y) < 1e-9 &&
                  std::abs(afterDrag.z - clicked.z) < 1e-9,
              "a drag never places it: that is the orbit");
        echo->clickProbe(0.3, 0.2);
        check(std::abs(echo->probeDirection().x - clicked.x) > 1e-6 ||
                  std::abs(echo->probeDirection().y - clicked.y) > 1e-6,
              "and a click elsewhere does move it");
    }

    std::printf("\nthe energy visualiser\n");
    {
        /*  End to end: audio in, a covariance pair out of the audio thread, a picture on the editor's.
            The core's own maths is checked in bambi-tests; what is checked here is the plumbing. */
        BambiEchoProcessor proc;
        proc.setBusesLayout(layout(amb(3), juce::AudioChannelSet::stereo(), amb(3)));
        proc.prepareToPlay(kRate, 128);

        std::vector<float> added, arrived;
        check(!proc.takeCovariances(added, arrived), "nothing is published before any audio has run");

        //  A plane wave from the front, long enough for a 50 ms window to fill.
        PlayingHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(18, 128);
        juce::MidiBuffer midi;
        std::vector<double> y(16, 0.0);
        bambi::shSN3D({1.0, 0.0, 0.0}, 3, y);
        const auto play = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                head.time += 128;
                buffer.clear();
                for (int c = 0; c < 16; ++c) {
                    auto* x = buffer.getWritePointer(c);
                    for (int i = 0; i < 128; ++i)
                        x[i] = static_cast<float>(
                            y[static_cast<std::size_t>(c)] *
                            std::sin(6.2831853 * 220.0 * static_cast<double>(head.time + i) / kRate));
                }
                proc.processBlock(buffer, midi);
            }
        };
        /*  Nobody is looking: the picture is off until a window asks for it, and gathering for
            a picture nobody draws was 1.47 % of a core at order 7, in every instance, for good.
            Catches the audio thread gathering regardless, and gathering during an offline render,
            which nobody watches. */
        play(40);
        check(!proc.takeCovariances(added, arrived), "with nobody looking, the audio thread gathers nothing");
        proc.wantEnergy(true);
        proc.setNonRealtime(true);
        play(40);
        check(!proc.takeCovariances(added, arrived), "nor while the host renders offline, whoever is looking");
        proc.setNonRealtime(false);
        for (int b = 0; b < 40; ++b) {
            head.time = b * 128;
            buffer.clear();
            for (int c = 0; c < 16; ++c) {
                auto* x = buffer.getWritePointer(c);
                for (int i = 0; i < 128; ++i)
                    x[i] = static_cast<float>(y[static_cast<std::size_t>(c)] *
                                              std::sin(6.2831853 * 220.0 * (b * 128 + i) / kRate));
            }
            proc.processBlock(buffer, midi);
        }
        check(proc.takeCovariances(added, arrived), "a covariance pair is published once a window has filled");
        check(added.size() == static_cast<std::size_t>(bambi::covarianceSize(3)) && arrived.size() == added.size(),
              "both are the upper triangle at this order (" + juce::String(static_cast<int>(added.size())) + ")");
        check(!proc.takeCovariances(added, arrived), "and it is taken once: the same pair is not served twice");

        //  What arrived points where the wave came from. This is the plumbing, through the real engine.
        bambi::EnergyField field;
        field.prepare(3);
        field.sample({}, arrived);
        field.step(0.05);
        int best = 0;
        for (int t = 1; t < bambi::kEnergyTexels; ++t)
            if (field.energyAt(t, false) > field.energyAt(best, false)) best = t;
        check(bambi::dot(bambi::EnergyField::directionOf(best), bambi::Vec3{1.0, 0.0, 0.0}) > 0.98,
              "and the arriving field's energy points where the wave came from");

        //  The editor draws it, and stops drawing 1.6 s after the last covariance.
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* echo = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(echo);
        const juce::Image shot = echo->createComponentSnapshot(echo->getLocalBounds(), true, 1.0f);
        check(shot.isValid(), "the editor paints with the visualiser on");
    }

    std::printf("\nthe region in the scene: the shared checks\n");
    bambi::editor::checkRegions<BambiEchoProcessor>({[](bambi::editor::PluginEditor& e, bool open) {
                                                         if (auto* echo = dynamic_cast<BambiEchoEditor*>(&e))
                                                             echo->showTab(open ? 1 : 0);  // the regions tab
                                                     },
                                                     [](bambi::editor::PluginEditor& e) -> std::optional<bambi::Vec3> {
                                                         if (auto* echo = dynamic_cast<BambiEchoEditor*>(&e))
                                                             return echo->probeDirection();
                                                         return std::nullopt;
                                                     }},
                                                    [](bool ok, const juce::String& what) { check(ok, what); });

    bambi::editor::checkRegionIsNotASource<Processor, BambiEchoEditor>();

    std::printf("\nthe scene corner: regions always, and the energy switch\n");
    {
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);

        /*  What the corner reports is recorded while painting, as a tile's area is, so every
            read here paints first -- `tick` only asks for a repaint, which never arrives headless. */
        const auto settle = [&] {
            ed->tick();
            ed->paintNow();
        };

        //  a spot, so there is a region to show or hide; the regions tab open, so it is the open one
        proc.document().edit("region kind",
                             [](bambi::PluginState& st) { st.regions[0].shape.kind = bambi::RegionKind::Spot; });
        ed->showTab(1);
        settle();

        //  ---- the energy switch ---------------------------------------------------------------
        /*  "Every plugin has one, and it can be switched off." At order 7 the picture is 26 % of
            a core, so being able to turn it off matters. */
        /*  Off when a window opens, and not remembered: several windows open in a host then
            draw nothing costly unless each was asked to. Catches the default going back to on. */
        check(!ed->energyShown(), "the visualiser is off when a window opens");
        const auto feed = [&] {
            juce::AudioBuffer<float> buffer(18, 128);
            juce::MidiBuffer midi;
            std::vector<double> y(16, 0.0);
            bambi::shSN3D({1.0, 0.0, 0.0}, 3, y);
            for (int b = 0; b < 60; ++b) {
                buffer.clear();
                for (int c = 0; c < 16; ++c) {
                    auto* x = buffer.getWritePointer(c);
                    for (int i = 0; i < 128; ++i)
                        x[i] = static_cast<float>(y[static_cast<std::size_t>(c)] *
                                                  std::sin(6.2831853 * 220.0 * (b * 128 + i) / 48000.0));
                }
                proc.processBlock(buffer, midi);
                settle();
            }
        };
        feed();
        check(!ed->energyDrawn() && !proc.energyWanted(),
              "and while it is off nothing is drawn and nothing is gathered");
        ed->setEnergyShown(true);
        check(proc.energyWanted(), "switching it on tells the audio thread somebody is looking");
        feed();
        check(ed->energyDrawn(), "the visualiser draws while it is on");
        check(ed->clickCornerToggle(false), "the energy toggle is there to click");
        settle();
        //  Catches a toggle that only redraws itself: the switch has to reach the picture.
        check(!ed->energyDrawn() && !proc.energyWanted(),
              "and clicking it switches the visualiser off, and the gathering with it");
        ed->clickCornerToggle(false);
        feed();  // off means nothing was gathered meanwhile: a window's worth has to arrive again
        check(ed->energyDrawn(), "and again switches it back on");
        //  closing the window is nobody looking, whatever the switch said

        //  ---- regions always ------------------------------------------------------------------
        check(ed->regionsDrawn() == 1, "the open region is drawn");
        ed->showTab(0);  // close the regions tab: now it is a CLOSED slot
        settle();
        check(ed->regionsDrawn() == 1, "a closed slot is still drawn while `regions always` is on");
        check(ed->clickCornerToggle(true), "the regions toggle is there to click");
        settle();
        /*  Off shows only the region whose tab is open, and none is. The default is on because
            otherwise the blue is shaped by a region nobody can see. */
        check(ed->regionsDrawn() == 0, "and with it off, a closed slot is not");
        ed->showTab(1);
        settle();
        check(ed->regionsDrawn() == 1, "while the OPEN one is drawn either way");
        ed->clickCornerToggle(true);  // back ON, which is where it started
        settle();

        //  ---- how it behaves, which is the part the prototype asserts -------------------------
        //  Catches a hit area that is only the square: a label you cannot click looks broken.
        ed->showTab(0);
        settle();
        const bool shownBefore = ed->regionsDrawn() == 1;
        /*  Clicked where the label is drawn -- right-aligned at the panel's inset, in the strip --
            and not at the recorded hit area's edge: a hit area shrunk to the square alone would drag
            that click along with it and the check would never see the shrink. */
        const juce::Point<float> onLabel{
            static_cast<float>(ed->equirectWidth()) - static_cast<float>(bambi::ui::theme::scene::labelInset) - 2.0f,
            static_cast<float>(bambi::ui::theme::scene::labelStrip) / 2.0f};
        ed->clickEquirectAt(onLabel);
        settle();
        check(shownBefore && ed->regionsDrawn() == 0, "the LABEL clicks it, not only the square");
        ed->clickEquirectAt(onLabel);
        settle();

        /*  The ordering -- the toggles registered after the view's whole-area region, so they take
            the press -- has no check of its own, deliberately: the corner sits in the label strip,
            over no sphere, and the whole-area click is `placeAt`, which refuses a position off the
            sphere, so a probe-placement check could not see this ordering break. On the globe the
            ordering would show as an orbit; the corner, on the equirect, never orbits at all. */
    }

    bambi::editor::checkEffectRetrigger(spec);

    std::printf("\na region angle set back to zero takes its turn with it\n");
    {
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        proc.document().edit("region kind",
                             [](bambi::PluginState& st) { st.regions[0].shape.kind = bambi::RegionKind::Spot; });

        const auto id = [](const char* key) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(key)); };
        const auto set = [&](const char* key, float value) {
            if (auto* p = proc.hostParameter(id(key))) p->setValueNotifyingHost(p->convertTo0to1(value));
        };
        //  a rate running, and an angle deliberately away from its default
        set("region1.yaw_rate", 60.0f);
        set("region1.yaw", 30.0f);

        //  playing, and moving on: set to restart, a rate turns only while the transport runs
        PlayingHead head;
        proc.setPlayHead(&head);
        juce::AudioBuffer<float> buffer(18, 128);
        juce::MidiBuffer midi;
        const auto run = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                buffer.clear();
                proc.processBlock(buffer, midi);
                head.time += buffer.getNumSamples();
            }
            ed->tick();
        };
        run(200);
        const auto turnOf = [&] {
            return static_cast<double>(proc.diagnostics().regionTurn[0].load(std::memory_order_relaxed));
        };
        check(turnOf() > 0.3, "a yaw rate has turned the region");

        //  double-click the tile to put it back to its default
        ed->showTab(1);
        ed->setRegionTab(1);  // the orientation is on the transform sub-tab
        ed->tick();
        const auto box = ed->tileAreaOf(id("region1.yaw"));
        check(!box.isEmpty(), "its yaw tile is on the regions tab");
        ed->doubleClickTile(id("region1.yaw"));
        run(2);
        /*  The angle goes back to its default and the turn goes with it. Catches the half that is
            easy to get wrong: zeroing the angle and leaving the region where the rate carried it. */
        const auto yaw = proc.hostParameter(id("region1.yaw"));
        check(yaw != nullptr && std::abs(yaw->convertFrom0to1(yaw->getValue())) < 0.01f,
              "a double-click puts the angle back to zero");
        check(turnOf() < 0.05, "and the turn a rate had added goes with it");

        //  and only that axis: pitch's turn is its own
        set("region1.pitch_rate", 60.0f);
        run(200);
        const auto pitchTurn = [&] {
            return static_cast<double>(proc.diagnostics().regionTurn[1].load(std::memory_order_relaxed));
        };
        check(pitchTurn() > 0.3, "a pitch rate turns pitch");
        ed->doubleClickTile(id("region1.yaw"));
        run(2);
        //  Catches a mask that clears every axis rather than the one whose angle was restored.
        check(pitchTurn() > 0.3, "and zeroing YAW leaves pitch's turn alone");

        /*  A rate is not an angle. Putting a rate back to its default means stop turning, not go
            back: the region stays where it got to. Catches a key match that treats `pitch_rate` as
            `pitch` -- which reads the same for the first five characters. */
        const auto before = pitchTurn();
        ed->doubleClickTile(id("region1.pitch_rate"));
        run(2);
        const auto rate = proc.hostParameter(id("region1.pitch_rate"));
        check(rate != nullptr && std::abs(rate->convertFrom0to1(rate->getValue())) < 0.01f,
              "a rate double-clicked back to its default stops the turning");
        check(pitchTurn() >= before - 0.01, "and leaves the turn where it had got to");
    }

    std::printf("\nthe equirect is 2:1, so a round thing draws round\n");
    {
        /*  It spans 360 degrees across and 180 down. Any other ratio stretches the picture -- at
            1.205:1, a circle draws 1.66 times taller than wide. Asserted on the projection at the
            equator, where the mapping is isotropic and the answer is arithmetic rather than a
            matter of taste. */
        const auto vp = bambi::ui::sceneViewport(bambi::Projection::Equirect, 286, 286);
        check(std::abs(vp.rx - 2.0 * vp.ry) < 1e-9, "the drawn box is exactly twice as wide as it is tall");

        const auto at = [&](double azDeg, double elDeg) {
            const auto p = bambi::project(bambi::Projection::Equirect, {},
                                          bambi::fromAzEl(azDeg * bambi::kDeg2Rad, elDeg * bambi::kDeg2Rad));
            return juce::Point<double>{vp.screenX(p.x), vp.screenY(p.y)};
        };
        //  ten degrees across and ten degrees up, from the same point on the equator
        const auto centre = at(0.0, 0.0);
        const auto across = std::abs(at(10.0, 0.0).x - centre.x);
        const auto up = std::abs(at(0.0, 10.0).y - centre.y);
        check(std::abs(across - up) < 0.01,
              "and ten degrees across measures the same as ten degrees up, at the equator");

        //  the whole sphere still fits the panel: correcting the ratio must not draw outside it
        const auto edge = at(180.0, 90.0);
        check(edge.x >= 0.0 && edge.x <= 286.0 && edge.y >= 0.0 && edge.y <= 286.0,
              "and the whole sphere is inside the panel it was given");

        /*  Room for `b l f r b`, which sit outside the frame, in a gap above it centred in a box one
            strip tall. Without it, a picture that reaches the top of its area puts them in the label
            strip, on top of the corner toggles. */
        const auto lettersTop = [&](int w, int h) {
            const auto v = bambi::ui::sceneViewport(bambi::Projection::Equirect, w, h);
            return v.cy - v.ry - bambi::ui::theme::scene::axisLabelGap - bambi::ui::theme::scene::labelStrip / 2.0;
        };
        const auto strip = static_cast<double>(bambi::ui::theme::scene::labelStrip);
        check(lettersTop(286, 286) >= strip, "the axis letters clear the label strip in one panel");
        //  Catches a height that fills the panel and leaves the letters nowhere to go.
        check(lettersTop(584, 286) >= strip, "and in the full row, where the picture is tallest");

        //  ---- the full row ---------------------------------------------------------------------
        const auto one = bambi::ui::sceneViewport(bambi::Projection::Equirect, 286, 286);
        const auto row = bambi::ui::sceneViewport(bambi::Projection::Equirect, 584, 286);
        check(std::abs(row.rx - 2.0 * row.ry) < 1e-9, "the full row is 2:1 as well");
        /*  The row is 2.195:1, so a 2:1 picture in it is limited by its height and fills it. Catches
            a full view that is merely wider rather than one that uses the height it just gained. */
        check(row.ry > 1.7 * one.ry, "and it is far bigger than one panel's: it fills the height");
        const auto rowLayout = bambi::ui::sceneRow(18, 60, true);
        const auto sideBySide = bambi::ui::sceneRow(18, 60, false);
        check(rowLayout.globe.isEmpty() && !sideBySide.globe.isEmpty(), "full hides the globe; side by side does not");
        check(rowLayout.equirect.getWidth() > sideBySide.equirect.getWidth(),
              "and the equirect takes the width the globe gave up");

        //  ---- and the toggle that does it, through the painted corner ---------------------
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        ed->tick();
        ed->paintNow();
        const int narrow = ed->equirectWidthNow();
        check(ed->globeShown() && !ed->equirectFull(), "the globe is there to start with");
        check(ed->clickFull(), "the full toggle is there to click");
        ed->tick();
        ed->paintNow();
        /*  Catches a toggle that flips the flag without laying the row out again. */
        check(!ed->globeShown() && ed->equirectWidthNow() > narrow,
              "clicking it hides the globe and widens the equirect");
        check(ed->clickFull(), "and it is still there to click back");
        ed->tick();
        ed->paintNow();
        check(ed->globeShown() && ed->equirectWidthNow() == narrow, "which puts the globe back");
    }

    std::printf("\nthe header is the encoder's, and lists this plugin's kind\n");
    {
        const int joinTicks = 8;  // a few ticks is enough for a join to be seen
        auto first = std::make_unique<BambiEchoProcessor>();
        auto second = std::make_unique<BambiEchoProcessor>();
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }
        //  an encoder on the same bus: a kind this window must not offer
        bambi::LinkBus encoder;
        const bool encoderOpen =
            encoder.open(first->identity().session, bambi::Uuid::generate(), bambi::Product::Encoder);
        encoder.publishStatic({});
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }
        check(encoderOpen, "an encoder is on the bus beside two echoes");

        std::unique_ptr<juce::AudioProcessorEditor> editor(first->createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        ed->tick();
        ed->paintNow();
        /*  Catches the list carrying everything on the bus, and catches a filter that keeps nothing:
            the window must still list itself. */
        check(ed->instanceCount() == 2, "the echo's header lists the two echoes, not the encoder");

        /*  ---- and by the track's name -------------------------------------------------------
            The label a header draws is the user's name for the instance, or the track's when there
            is none. The node holds this for all three plugins. */
        juce::AudioProcessor::TrackProperties track;
        track.name = juce::String("delay bus");
        second->updateTrackProperties(track);
        for (int i = 0; i < joinTicks * 2; ++i) {
            first->linkTick();
            second->linkTick();
        }
        ed->tick();
        bool named = false;
        for (int i = 0; i < ed->instanceCount(); ++i)
            if (ed->instanceLabel(i) == "delay bus") named = true;
        //  Catches the name never reaching the bus, and a label that falls back when it should not.
        check(named, "an instance is labelled by its track's name, not by its id");

        /*  ---- selecting another instance shows and edits it ---------------------------------
            Every control in the window goes onto the selected instance. Set the other one's wet
            level to something this one has not got, so what is shown can only have come from it. */
        const auto wet = static_cast<bambi::ParamId>(bambi::echoParams().byKey("output.wet"));
        const auto setWet = [wet](BambiEchoProcessor& p, float db) {
            if (auto* q = p.hostParameter(wet)) q->setValueNotifyingHost(q->convertTo0to1(db));
        };
        setWet(*first, 0.0f);
        setWet(*second, -12.0f);
        for (int i = 0; i < joinTicks; ++i) {
            first->linkTick();
            second->linkTick();
        }

        const auto other = second->identity().instance;
        ed->selectInstance(other);
        for (int i = 0; i < 20; ++i)  // the controls arrive on the next tick or two
        {
            first->linkTick();
            second->linkTick();
            ed->tick();
        }
        ed->paintNow();
        check(ed->showingAnother() && ed->anotherReady(),
              "selecting another instance follows it, and its controls arrive");
        /*  Catches a window that selects an instance and goes on showing its own values under that
            instance's name. */
        check(std::abs(ed->shownValue(wet) + 12.0f) < 0.5f,
              "and the panel shows THAT instance's value, not this one's");

        /*  The energy picture is of the instance shown. The window asks the other instance over the
            bus; that one gathers only while it is asked and sends its summary, and this one gathers
            nothing for it. */
        {
            second->prepareToPlay(48000.0, 128);
            const auto feedSecond = [&] {
                juce::AudioBuffer<float> buffer(18, 128);
                juce::MidiBuffer midi;
                std::vector<double> y(16, 0.0);
                bambi::shSN3D({0.0, 1.0, 0.0}, 3, y);
                for (int b = 0; b < 40; ++b) {
                    buffer.clear();
                    for (int c = 0; c < 16; ++c)
                        for (int i = 0; i < 128; ++i)
                            buffer.setSample(c, i,
                                             static_cast<float>(y[static_cast<std::size_t>(c)] *
                                                                std::sin(6.2831853 * 220.0 * (b * 128 + i) / 48000.0)));
                    second->processBlock(buffer, midi);
                    if (b % 8 == 7) {
                        first->linkTick();
                        second->linkTick();
                        ed->tick();
                        ed->paintNow();
                    }
                }
            };
            //  Catches the switch staying greyed, as it was while a field's energy never crossed the bus.
            check(ed->energyAvailable(), "while another instance is shown, the energy switch is live");
            feedSecond();
            //  Catches an owner that gathers for nobody: the ask is what turns it on.
            check(!second->energyWantedElsewhere() && !ed->energyDrawn(),
                  "off, the other instance is not asked to gather");
            ed->setEnergyShown(true);
            ed->tick();
            second->linkTick();
            //  Catches the ask never landing, and this instance gathering for somebody else's picture.
            check(second->energyWantedElsewhere() && !first->energyWanted() && !second->energyWanted(),
                  "on, the shown instance is asked over the bus -- and neither gathers for its own window");
            feedSecond();
            //  Catches the summary never being published or read, and a window drawing from its own processor.
            const auto alpha = ed->sceneView().energy.alpha();
            const bool lit = std::any_of(alpha.begin(), alpha.end(), [](float a) { return a > 0.0f; });
            check(ed->energyDrawn() && lit, "and its picture arrives and is drawn here");
            ed->setEnergyShown(false);
            ed->tick();
            //  The ask runs a second ahead: once it lapses, the other instance stops gathering.
            for (int i = 0; i < 70; ++i) {
                juce::Thread::sleep(20);
                second->linkTick();
                if (!second->energyWantedElsewhere()) break;
            }
            check(!second->energyWantedElsewhere(), "switched off, the ask lapses and the other instance stops");
        }

        /*  Catches a window that clears its own region's turn while showing another's, instead of
            the shown instance's, and catches the command going nowhere if it is never unpacked. */
        const auto id = [](const char* k) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(k)); };
        ed->showTab(1);
        ed->setRegionTab(1);
        ed->doubleClickTile(id("region1.yaw"));
        for (int i = 0; i < 4; ++i) {
            first->linkTick();
            second->linkTick();
            ed->tick();
        }
        check(first->regionZerosPending(0) == 0, "an angle reset on another instance leaves this one's turn alone");
        check((second->regionZerosPending(0) & 1) != 0, "and clears the yaw turn of the instance it is shown on");

        //  coming back the way a person does: the header's icon and name are home
        check(ed->clickHome(), "the header's icon and name can be clicked");
        ed->tick();
        ed->paintNow();
        check(!ed->showingAnother() && std::abs(ed->shownValue(wet)) < 0.5f,
              "and home comes back to this instance's own");
        ed->clickHome();
        ed->tick();
        check(!ed->showingAnother(), "and on its own instance, home stays there");

        /*  A window that closes mid-drag ends the gesture, on whichever instance it was on. A press on
            a tile while another instance is shown begins a gesture on that instance's host parameter,
            over the bus; with the window gone nothing would ever end it, and the other track's
            automation would stay in write for good. Catches the shared window not releasing it. */
        struct Gestures final : juce::AudioProcessorListener {
            int index{-1}, begins{0}, ends{0};
            void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
            void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
            void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int i) override {
                begins += i == index;
            }
            void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int i) override { ends += i == index; }
        } gestures;
        const auto blur = static_cast<bambi::ParamId>(bambi::echoParams().byKey("tap1.blur"));
        if (auto* q = second->hostParameter(blur)) gestures.index = q->getParameterIndex();
        second->addListener(&gestures);
        const auto settle = [&] {
            for (int i = 0; i < 6; ++i) {
                first->linkTick();
                second->linkTick();
                if (editor != nullptr) ed->tick();
            }
        };
        ed->selectInstance(other);
        settle();
        ed->showTab(0);
        ed->showCategory(2);
        const auto tile = ed->tileAreaOf(blur);
        ed->panelPressAt(tile.getCentre());  // pressed, and never released
        settle();
        const int begun = gestures.begins, endedWhileHeld = gestures.ends;
        editor.reset();
        settle();
        check(begun == 1 && endedWhileHeld == 0 && gestures.ends == 1,
              "a window closed mid-drag ends the gesture it began on the other instance (begins " +
                  juce::String(begun) + ", ends " + juce::String(endedWhileHeld) + " then " +
                  juce::String(gestures.ends) + ")");
        second->removeListener(&gestures);
        bambi::LinkBus::unlinkSession(first->identity().session);
    }

    std::printf("\na source's settings open from its matrix column\n");
    {
        /*  An effect's LFOs and envelopes need a settings page to open when clicked. The page is
            `ui/SourceSettings`, the same one the encoder draws, keyed by name because all three
            plugins stamp the same generator block. */
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        ed->tick();

        const auto id = [](const char* key) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(key)); };
        check(ed->shownSource() < 0, "no source is open to start with");

        //  ---- the LFO -----------------------------------------------------------------------
        //  through the column: this is the path that opens a source's settings
        ed->clickSourceColumn(bambi::MatrixTab::Generators, 0);
        ed->tick();
        ed->paintNow();
        check(ed->shownSource() == 12, "clicking lfo 1's matrix column opens its settings");
        /*  Its own parameters are reachable on the page -- by key, which is the whole reason one page
            serves three plugins whose enums put these somewhere different. */
        check(ed->drawnSomewhere(id("lfo1.rate")), "its rate is drawn on it");
        check(ed->drawnSomewhere(id("lfo1.shape")), "and its wave, which is a group of segments");
        //  polarity sits last, in a row of its own, on the LFO's flat page
        check(ed->drawnSomewhere(id("lfo1.polarity")), "and polarity, which 0099 gave it");

        //  ---- the envelope, and its two sub-tabs ---------------------------------------------
        ed->clickSourceColumn(bambi::MatrixTab::Generators, bambi::kNumLfos);
        ed->tick();
        ed->paintNow();
        check(ed->shownSource() == 15, "and clicking env 1's opens its own");
        check(ed->drawnSomewhere(id("env1.attack")), "its attack is drawn");
        //  Catches the curve label being drawn as text and never made draggable.
        check(ed->drawnSomewhere(id("env1.attack_curve")), "and its curve, under the number it shapes");
        check(!ed->drawnSomewhere(id("env1.source_dummy_never")), "a key that is not there is not drawn");

        /*  `shape` and `trigger` are sub-tabs: what the shape tab draws is not what the trigger tab
            draws, which is the whole argument for splitting them. */
        ed->setEnvelopeTab(1);
        ed->paintNow();
        check(!ed->drawnSomewhere(id("env1.attack")), "switching to trigger takes the shape's tiles away");
        ed->setEnvelopeTab(0);
        ed->paintNow();
        check(ed->drawnSomewhere(id("env1.attack")), "and switching back brings them");

        //  ---- stepping, which must not reach a page this plugin does not have -----------------
        ed->openSource(bambi::kNumSources - 2);  // the last envelope
        ed->stepSource(1);
        ed->paintNow();
        /*  The region is a source in the encoder and not here, so stepping wraps before it
            rather than opening a page whose column the matrix does not even offer. */
        check(ed->shownSource() == 0, "stepping past the last envelope wraps to the first feature");

        /*  Catches a column that only ever opens: the same source again closes its page and goes
            back to the tab it replaced, in every plugin. */
        ed->showTab(1);
        ed->clickSourceColumn(bambi::MatrixTab::Generators, 0);
        ed->paintNow();
        check(ed->shownSource() == 12, "a column opens its source");
        ed->clickSourceColumn(bambi::MatrixTab::Generators, 0);
        ed->paintNow();
        check(ed->shownSource() < 0 && ed->shownTab() == 1,
              "and the same column again closes it, back to the tab it replaced");

        /*  Catches a panel switching tabs by its own hand and leaving the source open behind it. */
        ed->clickSourceColumn(bambi::MatrixTab::Generators, 0);
        ed->showTab(0);
        ed->paintNow();
        check(ed->shownSource() < 0 && ed->shownTab() == 0, "choosing another tab closes a source's page");
        //  Two tabs of its own since ducking became a matrix row: there is no third to land on.
        ed->showTab(2);
        check(ed->shownTab() == 1, "and the panel has two tabs of its own, no empty third");
    }

    bambi::editor::checkEffectViewPresets<Processor, BambiEchoEditor>();

    std::printf("\nthe window is designed at one size and scaled, in an effect as in the encoder\n");
    {
        /*  Every plugin's window scales the same way. Catches the transform going -- a window made
            half as wide would then show the top-left quarter of itself -- by drawing the window at
            both sizes and asking where the same tile landed: half as far along, and a click there
            still reaches it. */
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        const auto id = [](const char* k) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(k)); };
        const int fullWidth = ed->getWidth(), fullHeight = ed->getHeight();
        check(ed->isResizable(), "an effect's window can be resized");
        ed->setSize(fullWidth / 2, fullHeight / 2);
        const juce::Image half = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
        const juce::Image full = [&] {
            ed->setSize(fullWidth, fullHeight);
            return ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
        }();
        //  the level column's rule is the right-most dark line: at half size it is at half the distance
        const auto darkestColumn = [](const juce::Image& image) {
            const int y = image.getHeight() / 2;
            int at = -1;
            float least = 2.0f;
            for (int x = image.getWidth() * 3 / 4; x < image.getWidth(); ++x)
                if (const float b = image.getPixelAt(x, y).getBrightness(); b < least) {
                    least = b;
                    at = x;
                }
            return at;
        };
        const int atFull = darkestColumn(full), atHalf = darkestColumn(half);
        check(atFull > 0 && std::abs(atHalf * 2 - atFull) <= 2,
              "at half the size everything is at half the distance (" + juce::String(atHalf) + " against " +
                  juce::String(atFull) + ")");
        ed->setSize(fullWidth / 2, fullHeight / 2);
        ed->showCategory(2);  // every pass: where blur's tile is
        check(ed->touchParameter(id("tap1.blur")) && ed->provisionalRow() == id("tap1.blur"),
              "and a tile still takes its click there");
    }

    std::printf("\na note after a block's last control step is not lost\n");
    {
        /*  The grid steps every 256 samples; a block of 512 has its last step at 256. A note at 300
            belongs to the next block's first step, so the grid carries it forward -- otherwise whether
            an envelope fired in an effect would depend on where the host cut its blocks. Catches the
            carry going. Env 1 is slot 15; its default trigger is a MIDI note, one-shot. */
        const auto envelopeAfter = [](int noteAt, int block, bool locateAfterFirstBlock = false) {
            BambiEchoProcessor proc;
            proc.setBusesLayout(layout(juce::AudioChannelSet::ambisonic(1), juce::AudioChannelSet::disabled(),
                                       juce::AudioChannelSet::ambisonic(1)));
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, block);
            proc.prepareToPlay(kRate, block);
            const int note = proc.document().editing().envTriggers[0].noteLow;
            juce::AudioBuffer<float> buffer(4, block);
            float peak = 0.0f;
            for (juce::int64 at = 0; at < 2048; at += block) {
                //  a locate: the host's timeline is somewhere else entirely from the second block on
                head.time = locateAfterFirstBlock && at > 0 ? at + 480000 : at;
                buffer.clear();
                juce::MidiBuffer midi;
                if (noteAt >= at && noteAt < at + block)
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), static_cast<int>(noteAt - at));
                proc.processBlock(buffer, midi);
                peak = std::max(peak, static_cast<bambi::host::TargetHost&>(proc).liveSourceValue(15));
            }
            return peak;
        };
        const float onAStep = envelopeAfter(256, 512);
        const float afterTheLast = envelopeAfter(300, 512);
        check(onAStep > 0.1f, "a note on a control step fires the envelope (" + juce::String(onAStep, 3) + ")");
        check(afterTheLast > 0.1f,
              "and so does one after the block's last step (" + juce::String(afterTheLast, 3) + ")");
        /*  Catches a carried note outliving a locate: it belongs to a timeline that is no longer
            playing, and firing it at the first step somewhere else is a trigger nobody played. */
        const float acrossALocate = envelopeAfter(300, 512, true);
        check(juce::exactlyEqual(acrossALocate, 0.0f),
              "a note carried out of a block is dropped by a locate (" + juce::String(acrossALocate, 3) + ")");
        //  and where the host cuts its blocks moves nothing: the same note, heard at the same step
        check(juce::exactlyEqual(envelopeAfter(300, 512), envelopeAfter(300, 64)),
              "the same note in blocks of 64 reaches the same value");
    }

    std::printf("\nlevel.release reaches this plugin's detector\n");
    {
        //  A burst falling to a quiet tone, above the silence gate, so the release is the whole fall.
        //  Catches this processor never handing the parameter to its detector; the detector itself
        //  is `core`'s, and the field's power is checked where there is a field to cancel (Reverb).
        const auto levelAfter = [](float releaseMs) {
            BambiEchoProcessor proc;
            proc.setBusesLayout(layout(juce::AudioChannelSet::ambisonic(1), juce::AudioChannelSet::disabled(),
                                       juce::AudioChannelSet::ambisonic(1)));
            PlayingHead head;
            proc.setPlayHead(&head);
            proc.setRateAndBufferSizeDetails(kRate, 256);
            setParameter(proc, "level.release", releaseMs);
            proc.prepareToPlay(kRate, 256);
            juce::AudioBuffer<float> buffer(4, 256);
            juce::MidiBuffer midi;
            const auto loud = static_cast<juce::int64>(0.5 * kRate), end = static_cast<juce::int64>(0.75 * kRate);
            for (juce::int64 at = 0; at < end; at += 256) {
                head.time = at;
                buffer.clear();
                for (int i = 0; i < 256; ++i) {
                    const double t = static_cast<double>(at + i) / kRate;
                    buffer.setSample(
                        0, i, (at + i < loud ? 0.5f : 0.01f) * static_cast<float>(std::sin(6.2831853 * 300.0 * t)));
                }
                proc.processBlock(buffer, midi);
            }
            return static_cast<bambi::host::TargetHost&>(proc).liveSourceValue(0);
        };
        const float fast = levelAfter(30.0f), slow = levelAfter(1500.0f);
        check(fast < 0.35f && slow > 0.6f, "level.release sets how fast level falls (" + juce::String(fast, 3) +
                                               " against " + juce::String(slow, 3) + ")");
    }

    std::printf("\na provisional row stops being one when it has a row of its own\n");
    {
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        const auto id = [](const char* k) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(k)); };
        const auto blur = id("tap1.blur");
        ed->showCategory(2);
        check(ed->touchParameter(blur) && ed->provisionalRow() == blur, "touching a tile opens a provisional row");
        proc.document().edit("depth", [blur](bambi::PluginState& s) {
            bambi::setCellDepth(bambi::echoMod(), s, bambi::MatrixTab::Generators, 0, blur, 0.5);
        });
        ed->tick();
        /*  Catches the row staying provisional for ever, its tile highlighted as if the assign flow
            were still open. */
        check(ed->provisionalRow() == bambi::kNoParamId, "a row with depth is no longer provisional");
    }

    std::printf("\nan envelope's shape is dragged, not only typed\n");
    {
        /*  The points set times and the sustain level; the diamonds at each stage's midpoint set
            nothing but its curve. */
        BambiEchoProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        auto* ed = dynamic_cast<BambiEchoEditor*>(editor.get());
        REQUIRE_EDITOR(ed);
        proc.document().edit("held", [](bambi::PluginState& s) {
            s.envTriggers[0].gate = bambi::TriggerGate::Held;  // so the plateau is there to take
        });
        ed->tick();
        ed->openSource(15);
        ed->paintNow();

        const auto id = [](const char* key) { return static_cast<bambi::ParamId>(bambi::echoParams().byKey(key)); };
        const auto of = [&](const char* key) { return ed->shownValue(id(key)); };

        check(!ed->envelopePlot().isEmpty(), "the graph reports where it drew itself");

        //  ---- the points -------------------------------------------------------------------
        const auto attack = of("env1.attack");
        check(ed->dragEnvelope(bambi::EnvelopeGrab::Attack, 40.0f, 0.0f), "the attack point takes a drag");
        check(of("env1.attack") > attack, "dragging it right lengthens the attack");
        /*  x is a stage's own time rather than a fraction of the live total, so every other point
            must stay exactly where it was. */
        check(std::abs(of("env1.decay") - 200.0f) < 0.01f, "and moves neither the decay");
        check(std::abs(of("env1.release") - 400.0f) < 0.01f, "nor the release");

        //  the handle ends up under the cursor, which is what "solved absolutely" buys
        const auto wanted = ed->envelopeHandle(bambi::EnvelopeGrab::Attack);
        ed->paintNow();
        check(std::abs(ed->envelopeHandle(bambi::EnvelopeGrab::Attack).x - wanted.x) < 1.5f,
              "and lands where the mouse left it, not near it");

        const auto sustain = of("env1.sustain");
        check(ed->dragEnvelope(bambi::EnvelopeGrab::DecaySustain, 0.0f, -20.0f), "the decay corner takes one");
        check(of("env1.sustain") > sustain, "dragging it up raises the sustain");

        //  the hold's right edge is a preview width: it sets the level and no time at all
        const auto release = of("env1.release");
        const auto level = of("env1.sustain");
        check(ed->dragEnvelope(bambi::EnvelopeGrab::SustainEdge, 60.0f, 20.0f), "the hold's edge takes one");
        check(of("env1.sustain") < level, "dragging it down lowers the sustain");
        check(std::abs(of("env1.release") - release) < 0.01f, "and its sideways travel sets no time");

        /*  A point dragged far past the edge sticks to it rather than solving a valid value that
            renders off the plot. Looked at while the mouse is down: the scale is frozen for the
            duration of a gesture, so after release the plot has grown to fit and the picture is
            settled either way. */
        ed->paintNow();
        ed->panelPressAt(ed->envelopeHandle(bambi::EnvelopeGrab::Release));
        ed->panelMoveTo({ed->envelopePlot().getRight() + 4000.0f, ed->envelopeHandle(bambi::EnvelopeGrab::Release).y});
        check(ed->envelopeHandle(bambi::EnvelopeGrab::Release).x <= ed->envelopePlot().getRight() + 1.0f,
              "a point dragged past the edge stays on the plot, mid-drag");
        ed->panelRelease();
        check(ed->envelopeHandle(bambi::EnvelopeGrab::Release).x <= ed->envelopePlot().getRight() + 1.0f,
              "and the plot has grown to fit it once the mouse is up");

        //  ---- the curve handles --------------------------------------------------------------
        /*  One sign convention for all three would bend two of them away from the mouse, because a
            falling stage dips below its chord as its curve shrinks. Checked as the visible outcome --
            the handle rising -- and not as the sign. */
        for (const auto* stage : {"attack", "decay", "release"}) {
            const auto grab = std::string(stage) == "attack"  ? bambi::EnvelopeGrab::AttackCurve
                              : std::string(stage) == "decay" ? bambi::EnvelopeGrab::DecayCurve
                                                              : bambi::EnvelopeGrab::ReleaseCurve;
            ed->paintNow();
            const auto before = ed->envelopeHandle(grab).y;
            ed->dragEnvelope(grab, 0.0f, -25.0f);
            check(ed->envelopeHandle(grab).y < before - 1.0f,
                  std::string("an upward drag raises the ") + stage + " curve");
        }
        const auto bowedDecay = of("env1.decay");
        ed->dragEnvelope(bambi::EnvelopeGrab::DecayCurve, 0.0f, -25.0f);
        check(std::abs(of("env1.decay") - bowedDecay) < 0.01f, "and a curve handle sets no time");

        //  ---- the axis polarity relabels, on both kinds of source ------------------------------
        /*  One grid draws both the LFO's preview and its axis, and the convention it shares with the
            curve is what the zero row checks -- a sine at phase 0 crosses zero exactly at the canvas's
            middle, and the grid's own "0" must land on the same pixel row or the two disagree. */
        ed->openSource(12);
        ed->paintNow();
        const auto wave = ed->wavePlot();
        check(!wave.area.isEmpty(), "the lfo's preview reports its canvas");
        check(wave.zeroRow.has_value(), "and its grid puts a zero on it");
        if (wave.zeroRow)
            check(std::abs(*wave.zeroRow - wave.area.getCentreY()) < 1.0f,
                  "where a sine at phase 0 crosses: the grid and the wave share one convention");

        //  polarity relabels the axis and moves nothing drawn: the same canvas, a different zero
        const auto lfoPolarity = id("lfo1.polarity");
        ed->shownValue(lfoPolarity);
        proc.hostParameter(lfoPolarity)->setValueNotifyingHost(0.0f);
        ed->tick();
        ed->paintNow();
        const auto unipolar = ed->wavePlot();
        check(unipolar.area == wave.area, "unipolar leaves the canvas exactly where it was");
        check(unipolar.zeroRow.has_value() && *unipolar.zeroRow > wave.area.getCentreY() + 1.0f,
              "and moves its zero to the floor, which is what polarity means");

        //  ---- and nothing is grabbed from empty space ----------------------------------------
        const auto plot = ed->envelopePlot();
        const auto quiet = of("env1.sustain");
        ed->panelDragTo({plot.getRight() - 6.0f, plot.getY() + 6.0f},
                        {plot.getRight() - 6.0f, plot.getBottom() - 6.0f});
        ed->paintNow();
        check(std::abs(of("env1.sustain") - quiet) < 1e-6f, "a drag on empty plot changes nothing");
    }

    //  ---- presets: the shared processor's and the shared window's, so the checks are shared too
    std::printf("\npresets\n");
    bambi::editor::checkPresets<BambiEchoProcessor>({"tap1.blur"},
                                                    [](bool ok, const juce::String& what) { check(ok, what); });

    return checkVerdict();
}
