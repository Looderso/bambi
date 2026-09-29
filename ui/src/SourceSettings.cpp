// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/SourceSettings.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>

#include "bambi/mod/matrix.hpp"
#include "bambi/mod/sources.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

/// A parameter by key, so the page never names one plugin's enum.
ParamId id(const ParameterPage& page, const std::string& key) {
    return static_cast<ParamId>(page.model().manifest().byKey(key));
}
ParamId numbered(const ParameterPage& page, const char* stem, int n, const char* field) {
    return id(page, std::string(stem) + std::to_string(n + 1) + "." + field);
}
float value(const ParameterPage& page, ParamId at) { return page.model().valueOf(at); }
ParamId amountOf(const ParameterPage& page, MatrixTab tab, int column) {
    return sourceColumn(page.model().modManifest(), tab, column).amount;
}
void setChoice(ParameterPage& page, ParamId at, int index) {
    page.model().changeParameter(
        at, toNormalised(page.model().manifest(), static_cast<int>(at), static_cast<float>(index)));
}

/// An outline from the core, drawn into `area`: x across it, y from `low` at the bottom to `high` at the top.
template <typename Points>
juce::Path outlinePath(juce::Rectangle<float> area, const Points& points, double low, double high) {
    juce::Path path;
    bool first = true;
    for (const auto& p : points) {
        const auto x = area.getX() + area.getWidth() * static_cast<float>(p.x);
        const auto y = area.getBottom() - area.getHeight() * static_cast<float>((p.y - low) / (high - low));
        if (first)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
        first = false;
    }
    return path;
}

//  live output, straight under the page's selector: level is the one reading never to hide behind a
//  tab, and what a source is sending is its level
float paintOutput(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y, int slot,
                  bool bipolar) {
    const auto v = page.model().sourceValueOf(slot);
    const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), ctl::tileNameHeight};
    text(g, "output", labelFont(), colour::label, row);
    text(g, fromCore(formatNumber(v, 2)), labelFont(), colour::output(), row, juce::Justification::centredRight);

    const auto top = row.getBottom() + ctl::tileValueGap;
    const auto clamped = std::clamp(v, bipolar ? -1.0 : 0.0, 1.0);
    BarContent bar;
    bar.fillFrom = bipolar ? 0.5 : 0.0;  // a bipolar source swings about the middle
    bar.fillTo = bipolar ? 0.5 + 0.5 * clamped : clamped;
    bar.tick = bipolar;
    bar.ink = colour::output();
    drawValueBar(g, {content.getX(), top, content.getWidth(), ctl::barHeight}, bar);
    page.liveRegion() = {content.getX(), y, content.getWidth(), top + ctl::barHeight - y};
    page.noteContentBottom(top + ctl::barHeight + page.scroll());
    return top + ctl::barHeight + ctl::sectionGap;
}

//  polarity, last and in a row of its own. No tile is ever full width, this one included, and the
//  second column is left empty rather than paired with something that does not belong beside it.
float paintPolarity(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y, ParamId at) {
    y = page.paintGroupTitle(g, content, y, "polarity");
    page.addParameterTile(g, tileCell(content, y, 0), at, "polarity");
    return tilesBottom(y, 1) + static_cast<float>(theme::space::groupGap);
}

// ---- an LFO: flat, wave then timing, polarity last ----------------------------------------------

float paintLfo(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, int lfo) {
    const auto key = [&](const char* field) { return numbered(page, "lfo", lfo, field); };
    auto y = content.getY();

    //  ---- wave: the four shapes, then the preview ------------------------------------------
    const auto shapeIndex = std::clamp(juce::roundToInt(value(page, key("shape"))), 0, 3);
    const auto shape = static_cast<LfoShape>(shapeIndex);
    const bool sampleHold = shape == LfoShape::SampleHold;
    page.paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight},
                       {"sine", "triangle", "saw", "s&h"}, shapeIndex, true,
                       [&page, k = key("shape")](int choice) { setChoice(page, k, choice); });
    page.noteControl(key("shape"), {content.getX(), y, content.getWidth(), ctl::segmentHeight}, true);
    y += ctl::segmentHeight + ctl::curveGap;

    //  the same grid the envelope's graph draws, in the LFO's own gutter, so the polarity knob below
    //  labels an axis that is actually there. Three ticks, not five: on a 56 px canvas five stack
    //  into an unreadable heap.
    const juce::Rectangle<float> wave{content.getX() + ctl::envelopeGutter, y, content.getWidth() - ctl::envelopeGutter,
                                      ctl::curveHeight};
    page.wavePlot() = {wave, drawNiceGrid(g, wave, static_cast<double>(value(page, key("polarity"))), ctl::lfoTicks)};
    //  a cycle boundary where the envelope has a stage's edge, and the same colour for both
    g.setColour(colour::envelopeGrid);
    const auto cycles = sampleHold ? ctl::sampleHoldSteps : ctl::lfoCycles;
    for (int i = 1; i < cycles; ++i)
        g.drawVerticalLine(juce::roundToInt(wave.getX() + wave.getWidth() * static_cast<float>(i) / cycles),
                           wave.getY(), wave.getBottom());
    g.setColour(colour::rule);
    g.drawVerticalLine(juce::roundToInt(wave.getX()), wave.getY(), wave.getBottom());

    //  the raw played shape, always: polarity only relabels the axis and never moves the drawing.
    //  Converted to the curve's own [0, 1] once, by the same convention the grid uses.
    const auto points = lfoOutline(shape, static_cast<double>(value(page, key("phase"))), cycles, ctl::curvePoints);
    g.setColour(colour::output());
    g.strokePath(outlinePath(wave.reduced(0.0f, theme::stroke::curve), points, -1.0, 1.0),
                 juce::PathStrokeType(theme::stroke::curve));
    y = wave.getBottom() + ctl::sectionGap;
    y = paintOutput(page, g, content, y, sourceSlot(MatrixTab::Generators, lfo), true);

    //  ---- timing: where in time the wave is, static and moving alike -----------------------
    y = page.paintGroupTitle(g, content, y, "timing");
    const bool synced =
        page.paintSyncSwitch(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, key("sync"));
    y += ctl::segmentHeight + ctl::segmentGap;

    y = page.paintRetrigger(g, content, y, key("retrigger"));

    //  the timing not in use stays in view, greyed. Phase sits with rate rather than with the wave:
    //  both are about where in time it is, one static and one moving. Sample-and-hold latches on
    //  its own cycle, which a phase offset does not move.
    page.addParameterTile(g, tileCell(content, y, 0), key("rate"), "rate", !synced);
    page.addParameterTile(g, tileCell(content, y, 1), key("div"), "division", synced);
    page.addParameterTile(g, tileCell(content, y, 2), key("phase"), "phase", !sampleHold);
    page.addParameterTile(g, tileCell(content, y, 3), amountOf(page, MatrixTab::Generators, lfo), "amount");
    y = tilesBottom(y, 4) + static_cast<float>(theme::space::groupGap);

    return paintPolarity(page, g, content, y, key("polarity"));
}

// ---- an envelope: two sub-tabs, shape and trigger ------------------------------------------------

/*  The graph is drawn at `envelopeHeight` rather than `curveHeight`: a shape meant to be read
    closely needs more room than one you glance at. It is drawn from `envelopeCurve`, which samples
    the same `ease` the engine steps through, so what is drawn is what plays, bows included. The
    hold is a light fill rather than a stroked line, because its width is a preview convention and
    not a fifth stage with a number of its own. */
/// "linear", or which way it is bowed and by how much.
juce::String curveLabel(double curve) {
    if (std::abs(curve) < 0.04) return "linear";
    return juce::String(curve > 0.0 ? "exp " : "inv ") + fromCore(formatNumber(std::abs(curve), 2));
}

float paintEnvelopeShape(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y,
                         int envelope) {
    const auto key = [&](const char* field) { return numbered(page, "env", envelope, field); };
    const EnvelopeGraph::Ids ids{key("attack"),  key("attack_curve"), key("decay"),        key("decay_curve"),
                                 key("sustain"), key("release"),      key("release_curve")};
    y = page.envelopeGraph().paint(page, g, content, y, ids, static_cast<double>(value(page, key("polarity"))));

    /*  Each timed stage carries its curve under its bar, where a unit would go: one box per value
        still, so the shape of the stage is said in words beside the number it shapes, not given a
        tile of its own. It is the number for a shape the graph above is the control for, and is
        draggable for the same reason every other number is. Sustain is a level and has none.

        The rows are laid out here rather than through `tileCell`'s own grid, because each needs the
        extra line underneath and the grid has no room for it. */
    const std::array<const char*, 4> names{"attack", "decay", "sustain", "release"};
    const std::array<const char*, 4> curves{"attack_curve", "decay_curve", nullptr, "release_curve"};
    const auto column = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const auto line = theme::type::label * ctl::valueLineHeight;
    const auto pitch = tileHeight(theme::type::value) + line + ctl::tileRowGap;
    for (int i = 0; i < 4; ++i) {
        const juce::Rectangle<float> cell{content.getX() + static_cast<float>(i % 2) * (column + ctl::tileColumnGap),
                                          y + static_cast<float>(i / 2) * pitch, column,
                                          tileHeight(theme::type::value)};
        page.addParameterTile(g, cell, key(names[static_cast<std::size_t>(i)]), names[static_cast<std::size_t>(i)]);
        const auto* c = curves[static_cast<std::size_t>(i)];
        if (c == nullptr) continue;
        const auto at = key(c);
        const juce::Rectangle<float> row{cell.getX(), cell.getBottom() + ctl::tileValueGap, cell.getWidth(), line};
        text(g, curveLabel(static_cast<double>(value(page, at))), labelFont(), colour::label, row);
        page.addHostDrag(row, at, false);
        page.noteControl(at, row, true);
    }
    y += 2.0f * pitch - ctl::tileRowGap + static_cast<float>(theme::space::groupGap);

    page.addParameterTile(g, tileCell(content, y, 0), amountOf(page, MatrixTab::Generators, kNumLfos + envelope),
                          "amount");
    y = tilesBottom(y, 1) + static_cast<float>(theme::space::groupGap);
    return paintPolarity(page, g, content, y, key("polarity"));
}
float paintTrigger(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, float y, int envelope) {
    //  everything here is state, not host parameters: edits go through the undo stack
    const auto e = static_cast<std::size_t>(envelope);
    const auto& t = page.model().state().envTriggers[e];
    const auto defaults = EnvTrigger::defaults(envelope);
    const bool midi = t.input == TriggerInput::Midi;
    const auto key = [envelope](const char* field) { return "trigger." + std::to_string(envelope) + "." + field; };
    const auto values = [](juce::Rectangle<float> tile) { return tileBox(tile); };

    y = page.paintGroupTitle(g, content, y, "trigger");
    page.paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, {"midi", "audio"}, midi ? 0 : 1,
                       true, [&page, e](int choice) {
                           page.model().learnSource(-1);
                           page.model().applyEdit(
                               choice == 0 ? "midi trigger" : "audio trigger", [e, choice](PluginState& s) {
                                   s.envTriggers[e].input = choice == 0 ? TriggerInput::Midi : TriggerInput::Audio;
                               });
                       });
    y += ctl::segmentHeight + ctl::segmentGap;

    if (midi) {
        page.paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, {"held", "one-shot"},
                           t.gate == TriggerGate::Held ? 0 : 1, true, [&page, e](int choice) {
                               page.model().applyEdit("trigger gate", [e, choice](PluginState& s) {
                                   s.envTriggers[e].gate = choice == 0 ? TriggerGate::Held : TriggerGate::OneShot;
                               });
                           });
        y += ctl::segmentHeight + ctl::sectionGap;

        //  notes: the left half of the number drags the lowest, the right half the highest
        const auto notesTile = tileCell(content, y, 0);
        TileContent notes;
        notes.name = "notes";
        notes.value = fromCore(noteRangeName(t.noteLow, t.noteHigh));
        notes.fillFrom = t.noteLow / 128.0;
        notes.fillTo = (t.noteHigh + 1) / 128.0;
        drawTile(g, notesTile, notes);
        const auto notesArea = values(notesTile);  // left half the lowest note, right half the highest
        page.addStateDrag(
            notesArea.withWidth(notesArea.getWidth() / 2.0f), "trigger", key("low"), t.noteLow, ctl::noteDragSpan, 1.0,
            [e](PluginState& s, double v) { setTriggerLow(s.envTriggers[e], static_cast<int>(v)); },
            [e, defaults](PluginState& s) { setTriggerLow(s.envTriggers[e], defaults.noteLow); });
        page.addStateDrag(
            notesArea.withTrimmedLeft(notesArea.getWidth() / 2.0f), "trigger", key("high"), t.noteHigh,
            ctl::noteDragSpan, 1.0,
            [e](PluginState& s, double v) { setTriggerHigh(s.envTriggers[e], static_cast<int>(v)); },
            [e, defaults](PluginState& s) { setTriggerHigh(s.envTriggers[e], defaults.noteHigh); });

        const auto channelTile = tileCell(content, y, 1);
        TileContent channel;
        channel.name = "channel";
        channel.value = fromCore(channelName(t.channel));
        channel.fillTo = t.channel / 16.0;
        drawTile(g, channelTile, channel);
        const int nextChannel = (t.channel + 1) % 17;
        page.addStateDrag(
            values(channelTile), "trigger", key("channel"), t.channel, ctl::channelDragSpan, 1.0,
            [e](PluginState& s, double v) { setTriggerChannel(s.envTriggers[e], static_cast<int>(v)); },
            [e, defaults](PluginState& s) { setTriggerChannel(s.envTriggers[e], defaults.channel); },
            [&page, e, nextChannel] {
                page.model().applyEdit("trigger channel", [e, nextChannel](PluginState& s) {
                    setTriggerChannel(s.envTriggers[e], nextChannel);
                });
            });

        const auto velocityTile = tileCell(content, y, 2);
        TileContent velocity;
        velocity.name = "velocity";
        velocity.value = juce::String(juce::roundToInt(t.velocity * 100.0)) + " %";
        velocity.fillTo = t.velocity;
        drawTile(g, velocityTile, velocity);
        page.addStateDrag(
            values(velocityTile), "trigger", key("velocity"), t.velocity, ctl::unitDragSpan, ctl::triggerStep,
            [e](PluginState& s, double v) { setTriggerVelocity(s.envTriggers[e], v); },
            [e, defaults](PluginState& s) { setTriggerVelocity(s.envTriggers[e], defaults.velocity); }, {},
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false,
            {tileBar(velocityTile), 0.0, 1.0});

        //  learn: the next note played sets the notes and the channel
        //  greyed where a plugin has none, never hidden and never live-looking-and-dead
        const auto learnTile = tileCell(content, y, 3);
        const bool canLearn = page.model().canLearnSource();
        const bool listening = canLearn && page.model().sourceLearning() == envelope;
        text(g, "learn", labelFont(), colour::label, learnTile.withHeight(ctl::tileNameHeight));
        const juce::Rectangle<float> button{learnTile.getX(),
                                            learnTile.getY() + ctl::tileNameHeight + ctl::tileValueGap,
                                            learnTile.getWidth(), ctl::buttonHeight};
        if (listening) {
            g.setColour(colour::learning);
            g.fillRect(button);
        } else {
            g.setColour(canLearn ? colour::chosen : colour::inactive);
            g.drawRect(button, theme::stroke::rule);
        }
        const auto ink = listening ? colour::textInverse : (canLearn ? colour::text : colour::inactive);
        text(g, listening ? "play a note" : "listen", font(theme::type::body), ink, button,
             juce::Justification::centred);
        page.noteNamedArea("learn note", button, canLearn);  // for a check: where it is, and whether it is live
        if (canLearn)
            page.addClickArea(button, [&page, envelope, listening] {
                page.model().learnSource(listening ? -1 : envelope);
                page.model().notifyChanged();
            });
        return tilesBottom(y, 4);
    }

    y += ctl::sectionGap - ctl::segmentGap;

    const auto sourceTile = tileCell(content, y, 0);
    TileContent source;
    source.name = "source";
    source.value = sourceLabel(page.model().modManifest(), t.source);
    source.fillFrom = t.source / static_cast<double>(kTriggerSources);
    source.fillTo = (t.source + 1) / static_cast<double>(kTriggerSources);
    drawTile(g, sourceTile, source);
    const int nextSource = (t.source + 1) % kTriggerSources;
    page.addStateDrag(
        values(sourceTile), "trigger", key("source"), t.source, kTriggerSources, 1.0,
        [e](PluginState& s, double v) { setTriggerSource(s.envTriggers[e], static_cast<int>(v)); },
        [e, defaults](PluginState& s) { setTriggerSource(s.envTriggers[e], defaults.source); },
        [&page, e, nextSource] {
            page.model().applyEdit("trigger source",
                                   [e, nextSource](PluginState& s) { setTriggerSource(s.envTriggers[e], nextSource); });
        });

    const auto thresholdTile = tileCell(content, y, 1);
    TileContent threshold;
    threshold.name = "threshold";
    threshold.value = fromCore(formatNumber(t.threshold, 2));
    threshold.fillTo = t.threshold;
    drawTile(g, thresholdTile, threshold);
    page.addStateDrag(
        values(thresholdTile), "trigger", key("threshold"), t.threshold, ctl::unitDragSpan, ctl::triggerStep,
        [e](PluginState& s, double v) { setTriggerThreshold(s.envTriggers[e], v); },
        [e, defaults](PluginState& s) { setTriggerThreshold(s.envTriggers[e], defaults.threshold); }, {},
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false,
        {tileBar(thresholdTile), 0.0, 1.0});

    const auto hysteresisTile = tileCell(content, y, 2);
    TileContent hysteresis;
    hysteresis.name = "hysteresis";
    hysteresis.value = fromCore(formatNumber(t.hysteresis, 2));
    hysteresis.fillFrom = std::max(0.0, t.threshold - t.hysteresis);  // from where it lets go to where it fires
    hysteresis.fillTo = t.threshold;
    drawTile(g, hysteresisTile, hysteresis);
    page.addStateDrag(
        values(hysteresisTile), "trigger", key("hysteresis"), t.hysteresis, ctl::hysteresisDragSpan, ctl::triggerStep,
        [e](PluginState& s, double v) { setTriggerHysteresis(s.envTriggers[e], v); },
        [e, defaults](PluginState& s) { setTriggerHysteresis(s.envTriggers[e], defaults.hysteresis); });
    return tilesBottom(y, 3);
}

// ---- a feature -------------------------------------------------------------------------------

float paintFeature(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, int slot) {
    const auto ref = sourceAt(slot);
    const auto feature = static_cast<Feature>(ref.index);
    auto y = paintOutput(page, g, content, content.getY(), slot, false);

    const auto tab = ref.sidechain ? MatrixTab::Sidechain : MatrixTab::Features;
    page.addParameterTile(g, tileCell(content, y, 0), amountOf(page, tab, ref.index), "amount");
    //  level's release, the one setting a detector has: how long whatever it drives takes to let go,
    //  ducking first. By key, as the generators' are, because all three plugins stamp the same block.
    if (feature == Feature::Level) {
        const int at = page.model().manifest().byKey(ref.sidechain ? "sc_level.release" : "level.release");
        if (at != kNoParam) page.addParameterTile(g, tileCell(content, y, 1), static_cast<ParamId>(at), "release");
    }
    y = tilesBottom(y, 2) + static_cast<float>(theme::space::groupGap);

    y = page.paintGroupTitle(g, content, y, "calibration");
    for (const auto& fact : featureCalibration(feature)) {
        const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), ctl::factRowHeight};
        drawFactRow(g, row, fromCore(fact.label), fromCore(fact.value));
        y += ctl::factRowHeight;
    }
    return y;
}

}  // namespace

// ---- which page, and the envelope's two sub-tabs -----------------------------------------------

void paintSourceSettings(ParameterPage& page, juce::Graphics& g, juce::Rectangle<float> content, int slot,
                         const RegionSlot& region) {
    const auto ref = sourceAt(slot);
    switch (ref.kind) {
        case SourceKind::Feature: page.noteContentBottom(paintFeature(page, g, content, slot) + page.scroll()); break;
        case SourceKind::Lfo: page.noteContentBottom(paintLfo(page, g, content, ref.index) + page.scroll()); break;
        case SourceKind::Envelope: {
            auto y = content.getY();
            const int open = std::clamp(page.model().envelopeTab(), 0, 1);
            page.paintSegments(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, {"shape", "trigger"},
                               open, true, [&page](int choice) {
                                   page.model().setEnvelopeTab(choice);
                                   page.model().notifyChanged();
                               });
            y += ctl::segmentHeight + static_cast<float>(theme::space::groupGap);
            y = paintOutput(page, g, content, y, sourceSlot(MatrixTab::Generators, kNumLfos + ref.index), false);

            const auto bottom = open == 0 ? paintEnvelopeShape(page, g, content, y, ref.index)
                                          : paintTrigger(page, g, content, y, ref.index);
            page.noteContentBottom(bottom + page.scroll());
            break;
        }
        case SourceKind::Region: {
            auto y = paintRegionTabs(page, g, content, content.getY());
            y = paintOutput(page, g, content, y, sourceSlot(MatrixTab::Region, 0), false);
            page.noteContentBottom(paintRegionEditor(page, g, content, y, region) + page.scroll());
            break;
        }
        case SourceKind::None: break;
    }
}

}  // namespace bambi::ui
