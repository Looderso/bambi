// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panel.h"

#include <algorithm>
#include <cmath>

#include "bambi/echo/patterns.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/SourceSettings.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

constexpr const char* kTabNames[] = {"taps", "send"};  // the send is its tab, as Reverb's send and return are
constexpr const char* kCategories[] = {"timing", "axis", "every pass"};

}  // namespace

EchoPanel::EchoPanel(EchoControlState& state) : ParameterPage(state), state_(state) { setOpaque(true); }

void EchoPanel::paint(juce::Graphics& g) {
    clearRegions();
    liveRegion() = {};
    clearNameAreas();
    tapRows_ = {};

    //  the bar, the rule, the viewport, the remote notice and the temporary source tab: the page's
    const auto frame = paintTabbedFrame(g, {kTabNames[0], kTabNames[1]});
    if (frame.content.isEmpty()) return;

    const ContentClip clip(*this, g);  // the tab bar stays put; what it governs scrolls under it
    switch (state_.tab) {
        case 1: paintRegions(g, frame.content); break;
        case 3: paintSourceSettings(*this, g, frame.content, state_.source, {0, "region1", "send"}); break;
        default: paintTaps(g, frame.content); break;
    }
}

/*  The strip. Four rows, pinned, on one time axis. A row is:
 *
 *      [3] [24]        [ track: where its passes land ]        [ level ]
 *       |   |
 *       |   the on/off, carrying the tap's number
 *       the selection bar, drawn only when selected
 *
 *  The order of registration is the interaction: the row's own select region goes down first and
 *  the switch and the level go down after it, because `HitArea` hit-tests back to front. That is
 *  what makes the on/off reachable without selecting -- silencing a tap must not drag its panel
 *  open.
 */
float EchoPanel::paintStrip(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const auto& diag = state_.processor.diagnostics();
    //  the taps as they play, modulation included, not as they are set
    const auto content_ = stripContent(bambi::ui::patchAsPlayed(state_), diag.bpm.load(), diag.sampleRate.load(),
                                       std::max(1, state_.processor.engineOrder()));
    span_ = content_.spanSeconds;

    const auto rowHeight = static_cast<float>(theme::metrics::matrixRow);
    //  wide enough for the widest level the range can show, and no wider
    const auto levelWidth = textWidth(font(theme::type::body), "-40.0 dB") + 2.0f * ctl::tileBoxPadX;
    for (int i = 0; i < bambi::kEchoTaps; ++i) {
        const juce::Rectangle<float> row{content.getX(), y, content.getWidth(), rowHeight};
        tapRows_[static_cast<std::size_t>(i)] = row;

        //  first: the row selects. Everything registered after this lies on top of it.
        addRegion({row, {}, {}, {}, [this, i] { selectTap(i); }, {}});

        if (i == state_.tap) {
            g.setColour(colour::selected());
            g.fillRect(row.getX(), row.getY(), theme::stroke::selectionBar, row.getHeight());
        }

        const auto x0 = row.getX() + theme::stroke::selectionBar + static_cast<float>(theme::space::grid);
        const juce::Rectangle<float> switchBox{x0, row.getY() + (rowHeight - ctl::segmentHeight) / 2.0f,
                                               ctl::segmentHeight, ctl::segmentHeight};
        const auto onOff = state_.tapParam(i, "on");
        const bool on = state_.value(onOff) > 0.5f;
        g.setColour(on ? colour::chosen : colour::text);
        if (on)
            g.fillRect(switchBox);
        else
            g.drawRect(switchBox, theme::stroke::rule);
        text(g, juce::String(i + 1), font(theme::type::body), on ? colour::textInverse : colour::text, switchBox,
             juce::Justification::centred);
        switchAreas_[static_cast<std::size_t>(i)] = switchBox;
        noteControl(onOff, switchBox, true);
        //  after the row, so it takes the press instead of it.
        addRegion({switchBox,
                   {},
                   {},
                   {},
                   [this, onOff] {
                       model().changeParameter(onOff, model().valueOf(onOff) > 0.5f ? 0.0f : 1.0f);
                       model().notifyChanged();
                   },
                   {}});

        const juce::Rectangle<float> levelBox{row.getRight() - levelWidth, row.getY() + 3.0f, levelWidth,
                                              rowHeight - 6.0f};
        const juce::Rectangle<float> track{
            switchBox.getRight() + ctl::stripRowGapX, row.getY(),
            levelBox.getX() - ctl::stripRowGapX - switchBox.getRight() - ctl::stripRowGapX, rowHeight};
        paintTrack(g, track, content_, i);

        /*  The level as a compact cell -- the number in a box -- and not a full tile. A tile is a
            name, a number and a bar and needs about 48 px; a row is 28, so a full tile here would
            draw all three on top of each other and run off the panel's edge. "The tap's level, in
            a box, so it reads as a control and not as the tail of the sequence." */
        const auto level = state_.tapParam(i, "level");
        const auto& levelDesc = model().manifest()[static_cast<int>(level)];
        g.setColour(on ? colour::rule : colour::inactive);
        g.drawRect(levelBox, theme::stroke::rule);
        text(g, fromCore(bambi::formatParameter(levelDesc, static_cast<double>(state_.value(level)))), labelFont(),
             on ? colour::text : colour::inactive, levelBox.reduced(ctl::tileBoxPadX, 0.0f),
             juce::Justification::centredRight);
        //  after the row, so a drag edits the level and never selects.
        noteControl(level, levelBox, on);
        if (on) addHostDrag(levelBox, level, false);
        y += rowHeight;
    }

    //  the beat grid, under all four rows, because they share one axis
    const auto x0 = content.getX() + theme::stroke::selectionBar + static_cast<float>(theme::space::grid) +
                    ctl::segmentHeight + ctl::stripRowGapX;
    const auto trackWidth = content.getRight() - levelWidth - ctl::stripRowGapX - x0;
    const juce::Rectangle<float> ruler{x0, y, trackWidth, ctl::tileNameHeight};
    paintRuler(g, ruler, content_);
    y = ruler.getBottom();

    ruleH(g, content.getX(), content.getRight(), y, colour::rule);
    noteContentBottom(y + scroll());
    return y + ctl::sectionGap;
}

/*  A pass mark carries position, the level fall as opacity, the band as vertical extent, and vaguer
 *  as softness that compounds. Level is opacity rather than height, because height is the band's
 *  and two things cannot share one axis.
 *
 *  Vaguer softens horizontally only: the vertical axis is frequency, so blurring down it would
 *  smear the band into the blur.
 */
void EchoPanel::paintTrack(juce::Graphics& g, juce::Rectangle<float> track, const StripContent& content, int tapIndex) {
    const auto& tap = content.taps[static_cast<std::size_t>(tapIndex)];
    const bool selected = tapIndex == state_.tap;
    if (!tap.on || content.spanSeconds <= 0.0) return;

    const auto sampleRate = static_cast<double>(state_.processor.diagnostics().sampleRate.load());
    const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto h = track.getHeight();
    const auto ink = colour::valueFill;

    for (const auto& pass : tap.passes) {
        const auto at = static_cast<float>(pass.seconds / content.spanSeconds);
        if (at < 0.0f || at > 1.0f) continue;
        const auto x = track.getX() + at * track.getWidth();

        //  opacity: the pass's level, compressed. Without it a -6 dB feedback leaves nothing to see
        //  after two passes.
        auto alpha = static_cast<float>(std::pow(std::max(0.0, pass.gain), ctl::markLevelPower));
        if (!selected) alpha *= 0.33f;  // the selected tap in full, the rest at a third
        if (alpha < ctl::markFloor) continue;

        //  softness compounds as the square root of the passes, the law the audio follows
        const auto soften = static_cast<float>(tap.blurRad / bambi::kPi * std::sqrt(static_cast<double>(pass.index)));
        const auto halfWidth = theme::stroke::liveMark / 2.0f + soften * track.getWidth() * 0.02f;

        //  the band, down the row: 20 kHz at the top to 20 Hz at the bottom, compounded j times
        const int steps = juce::roundToInt(h);
        for (int yy = 0; yy < steps; ++yy) {
            const double t = steps <= 1 ? 0.0 : static_cast<double>(yy) / (steps - 1);
            const double hz = 20000.0 * std::pow(20.0 / 20000.0, t);
            const double band =
                std::pow(bandMagnitude(hz, tap.lowCutHz, tap.highCutHz, rate), static_cast<double>(pass.index));
            const auto a = alpha * static_cast<float>(band);
            if (a < ctl::markFloor) continue;
            g.setColour(ink.withAlpha(std::min(1.0f, a)));
            g.fillRect(x - halfWidth, track.getY() + static_cast<float>(yy), 2.0f * halfWidth, 1.0f);
        }
    }
}

/// Beats under the shared axis, so an offset can be read against them.
void EchoPanel::paintRuler(juce::Graphics& g, juce::Rectangle<float> ruler, const StripContent& content) {
    if (content.spanSeconds <= 0.0 || content.beatSeconds <= 0.0) return;
    const int beats = juce::roundToInt(content.spanSeconds / content.beatSeconds);
    for (int b = 0; b <= beats; ++b) {
        const auto at = static_cast<float>(static_cast<double>(b) * content.beatSeconds / content.spanSeconds);
        const auto x = ruler.getX() + at * ruler.getWidth();
        const bool bar = b % 4 == 0;
        g.setColour(bar ? colour::rule : colour::inactive);
        g.fillRect(x, ruler.getY(), theme::stroke::rule, bar ? ruler.getHeight() : ruler.getHeight() * 0.5f);
        if (bar)
            text(g, juce::String(b / 4 + 1), labelFont(), colour::label,
                 {x + 3.0f, ruler.getY(), 20.0f, ruler.getHeight()});
    }
}

/*  The pattern: one row of starting points above the taps. A pattern writes the four taps' pattern
    keys -- on, steps, offsets, axis, spin, skew, feedback -- and nothing else, which `applyPattern`
    decides, not this panel. The chosen pattern stays lit while the taps are edited; a double-click
    brings its values back. The strip under it is unchanged. */
float EchoPanel::paintPattern(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    std::vector<juce::String> names;  // no title: the row is the taps' starting points, and says so
    for (const auto n : bambi::kEchoPatternNames) names.push_back(fromCore(n));
    const auto apply = [this](int which) {
        //  through the host's own parameters, inside gestures, so it automates and undoes as the knobs would
        bambi::applyPattern(
            model().manifest(), static_cast<bambi::EchoPattern>(which),
            [this](int at, float normalised) { model().changeParameter(static_cast<bambi::ParamId>(at), normalised); });
        model().applyEdit("pattern", [which](bambi::PluginState& s) { s.echoPattern = which; });
        model().notifyChanged();
    };
    const auto row = paintPicker(g, content.getX(), y, content.getWidth(), names,
                                 std::clamp(state_.patch.echoPattern, 0, 4), true, apply, apply);
    return row.bottom + static_cast<float>(theme::space::groupGap);
}

void EchoPanel::paintTaps(juce::Graphics& g, juce::Rectangle<float> content) {
    auto y = paintPattern(g, content, content.getY());
    const auto stripTop = y;
    y = paintStrip(g, content, y);
    stripArea_ = {content.getX(), stripTop, content.getWidth(), y - stripTop};  // drawn from the taps as played

    const juce::Rectangle<float> tabs{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    paintSegments(g, tabs, {kCategories[0], kCategories[1], kCategories[2]}, state_.category, true,
                  [this](int index) { showCategory(index); });
    y = tabs.getBottom() + ctl::sectionGap;

    switch (state_.category) {
        case 1: y = paintAxis(g, content, y); break;
        case 2: y = paintEveryPass(g, content, y); break;
        default: y = paintTiming(g, content, y); break;
    }
    //  not here: wet and dry. They are the plugin's and not a tap's, so they are always on screen,
    //  in the level column down the right edge.
    juce::ignoreUnused(y);
}

/*  Timing. The synced and the free rows are both on screen whatever `sync` says, the inactive pair
    greyed and inert; swing sits on its own row under them, greyed when free. No tile name repeats:
    `steps` and `time` for the period, `offset steps` and `offset` for the offset. */
float EchoPanel::paintTiming(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const int tap = state_.tap;
    const bool synced =
        paintSyncSwitch(g, {content.getX(), y, content.getWidth(), ctl::segmentHeight}, state_.tapParam(tap, "synced"));
    y += ctl::segmentHeight + ctl::sectionGap;

    const auto pair = [&](bambi::ParamId freeId, const char* freeName, bambi::ParamId syncedId,
                          const char* syncedName) {
        addParameterTile(g, tileCell(content, y, 0), freeId, freeName, !synced);
        addParameterTile(g, tileCell(content, y, 1), syncedId, syncedName, synced);
        y = tilesBottom(y, 2) + ctl::tileRowGap;
    };
    pair(state_.tapParam(tap, "ms"), "time", state_.tapParam(tap, "steps"), "steps");
    pair(state_.tapParam(tap, "offset_ms"), "offset", state_.tapParam(tap, "offset_steps"), "offset steps");

    //  swing rides the offset, and only a synced tap has one
    addParameterTile(g, tileCell(content, y, 0), state_.tapParam(tap, "swing"), "swing", synced);
    y = tilesBottom(y, 1) + static_cast<float>(theme::space::groupGap);
    noteContentBottom(y + scroll());
    return y;
}

/// The axis a pass turns about. How far round it goes each time is `every pass`'s, beside skew: one place.
float EchoPanel::paintAxis(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const int tap = state_.tap;
    return paintGroup(g, content, y, {}, {state_.tapParam(tap, "az"), state_.tapParam(tap, "el")});
}

/*  `every pass` reads as two pairs, one to a row: `degrees` beside `skew` -- where each pass goes,
    along and around the axis -- and `feedback` beside `vaguer` -- how each pass differs from the
    one before it. The band follows beneath. The row is the pairing device the grid already has, so
    the pairs need no titles of their own: naming them costs 23 px and says nothing the row does
    not. */
float EchoPanel::paintEveryPass(juce::Graphics& g, juce::Rectangle<float> content, float y) {
    const int tap = state_.tap;
    y = paintGroup(g, content, y, {}, {state_.tapParam(tap, "spin"), state_.tapParam(tap, "skew")});
    y = paintGroup(g, content, y, {}, {state_.tapParam(tap, "feedback"), state_.tapParam(tap, "blur")});
    return paintGroup(g, content, y, "band", {state_.tapParam(tap, "low_cut"), state_.tapParam(tap, "high_cut")});
}

/*  Echo's one region slot, the send, with no send/return sub-selector since there is only one slot
    to switch between. The editor itself is the shared one. */
void EchoPanel::paintRegions(juce::Graphics& g, juce::Rectangle<float> content) {
    const auto y = paintRegionTabs(*this, g, content, content.getY());
    paintRegionEditor(*this, g, content, y, {0, "region1", "send"});
}

void EchoPanel::selectTap(int index) {
    //  Selecting never moves the tab, and switching tab never moves the selection.
    state_.tap = std::clamp(index, 0, bambi::kEchoTaps - 1);
    state_.notify();
}

void EchoPanel::showCategory(int index) {
    state_.category = std::clamp(index, 0, 2);
    state_.notify();
}

bool EchoPanel::touchParameter(bambi::ParamId id) {
    const auto area = nameArea(id);
    if (area.isEmpty()) return false;
    clickAt(area.getCentre());
    return true;
}

juce::Rectangle<float> EchoPanel::tapSwitch(int index) const {
    return index >= 0 && index < bambi::kEchoTaps ? switchAreas_[static_cast<std::size_t>(index)]
                                                  : juce::Rectangle<float>{};
}

juce::Rectangle<float> EchoPanel::tapRow(int index) const {
    return index >= 0 && index < bambi::kEchoTaps ? tapRows_[static_cast<std::size_t>(index)]
                                                  : juce::Rectangle<float>{};
}

}  // namespace bambi::ui
