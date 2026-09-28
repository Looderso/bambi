// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/ParameterPage.h"

#include <algorithm>
#include <cmath>

#include "bambi/mod/matrix.hpp"
#include "bambi/ui/Draw.h"
#include "bambi/ui/HostDrag.h"
#include "bambi/ui/RegionClipboard.h"
#include "bambi/ui/RegionEditor.h"
#include "bambi/ui/Tiles.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

bool isAmount(const ParamDesc& d) { return d.group == ParamGroup::Modulation && d.unit.empty(); }
}  // namespace

juce::String tileValueText(const ParamDesc& d, double value) {
    return isAmount(d) ? juce::String(juce::roundToInt(value * 100.0)) + " %" : fromCore(formatParameter(d, value));
}

ParameterPage::FrameContent ParameterPage::paintFrame(juce::Graphics& g, std::initializer_list<const char*> tabs,
                                                      int selected, const std::function<void(int)>& select,
                                                      const std::function<float(juce::Graphics&, float)>& extra) {
    g.fillAll(colour::ground);
    const auto width = static_cast<float>(getWidth());
    const auto height = static_cast<float>(getHeight());
    const auto inset = static_cast<float>(theme::space::inset);

    const auto tabFont = font(theme::type::body, theme::type::trackingTab);
    auto x = inset;
    int i = 0;
    for (const auto* name : tabs) {
        //  the name starts at `x`; the block reaches a pad either side of it
        const juce::Rectangle<float> box{x - ctl::tabPadX, 0.0f, textWidth(tabFont, name) + 2.0f * ctl::tabPadX,
                                         ctl::tabBarHeight};
        drawChoice(g, box, name, tabFont, i == selected);
        const int index = i;
        addRegion({box, {}, {}, {}, [select, index] { select(index); }, {}});
        x = box.getRight() + ctl::tabGap + ctl::tabPadX;
        ++i;
    }
    if (extra) x = extra(g, x);
    juce::ignoreUnused(x);
    ruleH(g, 0.0f, width, ctl::tabBarHeight, colour::ruleStrong);

    setViewport(ctl::tabBarHeight, height);
    const juce::Rectangle<float> full{inset, ctl::tabBarHeight + ctl::contentTop, width - 2.0f * inset,
                                      height - ctl::tabBarHeight - ctl::contentTop};

    //  another instance is selected and has not answered yet: there is nothing of its to draw, and
    //  the notice offers the way back
    if (model_.showingAnother() && !model_.anotherReady()) {
        drawRemoteNotice(g, full, model_.anotherLabel());
        addRegion({remoteNoticeLink(full), {}, {}, {}, [this] { model_.selectSelfInstance(); }, {}});
        return {{}, full};
    }

    //  the tab bar stays put and what it governs scrolls under it. The rect handed to a painter keeps
    //  the full height it would have had, so a tab still lays itself out whole and the viewport
    //  decides what of it shows
    return {full.withY(full.getY() - scroll()).withHeight(full.getHeight() + scroll()), full};
}

bool ParameterPage::paintSyncSwitch(juce::Graphics& g, juce::Rectangle<float> area, ParamId sync) {
    const bool synced = model_.valueOf(sync) > 0.5f;
    paintSegments(g, area, {"free", "sync"}, synced ? 1 : 0, true,
                  [this, sync](int index) { model_.changeParameter(sync, index > 0 ? 1.0f : 0.0f); });
    //  the segments are this parameter's box: it has no tile, but it is where a click changes it, so
    //  it is recorded like one. Otherwise a check counting what is reachable calls a control that is
    //  plainly on screen missing
    noteControl(sync, area, true);
    return synced;
}

void ParameterPage::followClipboard() {
    if (!pasteDrawn_) return;
    const auto held = regionOnClipboard();
    if ((held.has_value() ? describeRegionClip(*held) : std::string()) == pasteShown_) return;
    ++clipboardRepaintsAsked_;
    repaint();
}

float ParameterPage::paintRetrigger(juce::Graphics& g, juce::Rectangle<float> content, float y, ParamId retrigger,
                                    bool enabled) {
    const bool continues = model_.valueOf(retrigger) > 0.5f;
    const juce::Rectangle<float> area{content.getX(), y, content.getWidth(), ctl::segmentHeight};
    paintSegments(g, area, {"restart", "continue"}, continues ? 1 : 0, enabled,
                  [this, retrigger](int index) { model_.changeParameter(retrigger, index > 0 ? 1.0f : 0.0f); });
    noteControl(retrigger, area, enabled);  // it has no tile, but it is where a click changes it
    y += ctl::segmentHeight + ctl::sectionGap;
    if (continues && enabled) {
        const auto line = theme::type::label * ctl::noticeLineHeight;
        text(g, "runs on through play and locate: a bounce will differ", labelFont(), colour::label,
             {content.getX(), y, content.getWidth(), line});
        y += line + ctl::sectionGap;
    }
    noteContentBottom(y + scroll());
    return y;
}

float ParameterPage::paintHint(juce::Graphics& g, juce::Rectangle<float> content, const juce::String& hint) {
    const auto line = theme::type::label * ctl::noticeLineHeight;
    const auto f = labelFont();
    if (textWidth(f, hint) <= content.getWidth()) {
        text(g, hint, f, colour::label, content.withHeight(line));
        noteContentBottom(content.getY() + line + scroll());
        return content.getY() + line + ctl::hintGap;
    }
    //  a longer hint wraps onto a second line, and no further
    g.setColour(colour::label);
    g.setFont(f);
    const auto twoLines = 2.0f * line;
    g.drawFittedText(hint, content.withHeight(twoLines).toNearestInt(), juce::Justification::centredLeft, 2, 1.0f);
    noteContentBottom(content.getY() + twoLines + scroll());
    return content.getY() + twoLines + ctl::hintGap;
}

float ParameterPage::paintGroupTitle(juce::Graphics& g, juce::Rectangle<float> content, float y,
                                     const juce::String& title) {
    const auto next = drawGroupHeading(g, content.getX(), y, content.getWidth(), title);
    noteContentBottom(y + ctl::groupTitleHeight + scroll());
    return next;
}

float ParameterPage::paintGroup(juce::Graphics& g, juce::Rectangle<float> content, float y, const juce::String& title,
                                std::initializer_list<Cell> cells) {
    if (title.isNotEmpty()) y = paintGroupTitle(g, content, y, title);
    const auto columnWidth = (content.getWidth() - ctl::tileColumnGap) / 2.0f;
    const auto height = tileHeight(theme::type::value);
    int i = 0;
    for (const auto& cell : cells) {
        const auto column = static_cast<float>(i % 2);
        const auto row = static_cast<float>(i / 2);
        const juce::Rectangle<float> tile{content.getX() + column * (columnWidth + ctl::tileColumnGap),
                                          y + row * (height + ctl::tileRowGap), columnWidth, height};
        if (cell.custom)
            cell.custom(g, tile);
        else
            addParameterTile(g, tile, cell.id);
        ++i;
    }
    const auto rows = static_cast<float>((i + 1) / 2);
    return y + rows * (height + ctl::tileRowGap) - ctl::tileRowGap + static_cast<float>(theme::space::groupGap);
}

void ParameterPage::paintSegments(juce::Graphics& g, juce::Rectangle<float> area,
                                  std::initializer_list<const char*> names, int selected, bool enabled,
                                  const std::function<void(int)>& choose, int unavailable) {
    std::vector<juce::String> labels;
    for (const auto* name : names) labels.emplace_back(name);
    const auto cells = drawSegments(g, area, labels, selected, enabled, unavailable);
    for (int i = 0; i < static_cast<int>(cells.size()); ++i)
        if (enabled && i != unavailable && i != selected)
            addRegion({cells[static_cast<std::size_t>(i)], {}, {}, {}, [choose, i] { choose(i); }, {}});
    noteContentBottom(area.getBottom() + scroll());
}

ParameterPage::PickerLayout ParameterPage::paintPicker(juce::Graphics& g, float x, float y, float width,
                                                       const std::vector<juce::String>& names, int selected,
                                                       bool enabled, const std::function<void(int)>& choose,
                                                       const std::function<void(int)>& reselect, int unavailable,
                                                       int columns) {
    const auto cells = drawSegmentLines(g, x, y, width, names, selected, enabled, unavailable, columns);
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
        const auto& cell = cells[static_cast<std::size_t>(i)];
        if (!enabled || i == unavailable) continue;
        if (i != selected)
            addRegion({cell, {}, {}, {}, [choose, i] { choose(i); }, {}});
        else if (reselect)
            addRegion({cell, {}, {}, {}, {}, [reselect, i] { reselect(i); }});
    }
    const auto bottom = y + segmentLinesHeight(static_cast<int>(names.size()), columns);
    noteContentBottom(bottom + scroll());
    return {cells, bottom};
}

void ParameterPage::paintButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, bool enabled,
                                std::function<void()> click) {
    drawFramedButton(g, area, label, enabled);
    if (enabled) addRegion({area, {}, {}, {}, std::move(click), {}});
}

void ParameterPage::addHostDrag(juce::Rectangle<float> area, ParamId id, bool stepped, bool addsRow) {
    //  how a host parameter is moved is one statement, shared with the level column
    bambi::ui::addHostDrag(*this, model_, hostDrag_, area, id, stepped, addsRow, [this, id] { onDefaultRestored(id); });
}

void ParameterPage::addParameterTile(juce::Graphics& g, juce::Rectangle<float> tile, ParamId id,
                                     const juce::String& name, bool enabled) {
    const auto& m = model_.manifest();
    const auto& d = m[static_cast<int>(id)];
    const auto value = static_cast<double>(model_.valueOf(id));
    const bool target = isModulationTarget(model_.modManifest(), id);
    const bool committed = target && model_.committed(id);

    const bool amount = isAmount(d);
    TileContent t;
    t.name = name.isNotEmpty() ? name : fromCore(d.name);
    t.value = tileValueText(d, value);
    t.modulationInk = amount;
    t.enabled = enabled;
    //  a bipolar parameter fills from zero, so centred reads as centred -- a speed, a displacement,
    //  an angle. A level in decibels is not bipolar: 0 dB is unity and not a middle, and filling
    //  from it makes the bar shrink as the level rises, which reads as if the control cannot move
    //  the dry level at all. A dB range that happens to cross zero is still a magnitude
    const bool signedRange = d.min < 0.0f && d.max > 0.0f && d.unit != "dB";
    t.fillFrom = signedRange ? normalised(m, id, 0.0) : 0.0;
    t.tick = signedRange;
    t.fillTo = normalised(m, id, value);
    //  the second mark: what the engine is applying now. The number and the fill stay the parameter,
    //  so the thing you drag keeps saying what you set while the drift shows beside it. Compared
    //  in normalised space, so one rule covers an angle in degrees and a bare ratio alike
    double applied = 0.0;
    if (liveValue(id, applied)) {
        t.live = normalised(m, id, applied);
        t.hasLive = std::abs(t.live - t.fillTo) > 0.002;
    }
    if (committed) {
        const auto reach = modulationReach(model_.modManifest(), model_.state(), id);
        t.modFrom = normalised(m, id, value + reach.low);
        t.modTo = normalised(m, id, value + reach.high);
    }
    t.committed = committed;
    t.highlighted = model_.provisionalRow() == id;
    t.nameActive = committed || t.highlighted;
    /*  Registered exactly as it was drawn: drawTile knows where the live mark goes, so this does not
        have to guess at it. Empty unless something on this tile moves on its own. */
    live_ = live_.getUnion(drawTile(g, tile, t));
    noteContentBottom(tile.getBottom() + scroll());

    //  One box for the whole value: drag it to set, click it to give it a row.
    const auto box = tileBox(tile);
    tileAreas_[static_cast<std::size_t>(id)] = box;
    tileEnabled_[static_cast<std::size_t>(id)] = enabled;
    if (enabled) addHostDrag(box, id, d.type != ParamType::Float, target);
}

bool ParameterPage::repaintMoving() {
    bool moved = false;
    juce::Rectangle<float> area = live_;
    const int count = std::min(model_.manifest().size(), kMaxParams);
    for (int i = 0; i < count; ++i) {
        const auto at = static_cast<std::size_t>(i);
        double v = 0.0;
        const bool has = model_.engineValue(static_cast<ParamId>(i), v);
        const auto now = static_cast<float>(v);
        if (has != hadEngine_[at] || (has && now != lastEngine_[at])) {
            moved = true;
            area = area.getUnion(tileAreas_[at]);
        }
        hadEngine_[at] = has;
        lastEngine_[at] = has ? now : 0.0f;
    }
    if (moved) area = area.getUnion(enginePicture());
    if (!area.isEmpty()) {
        ++liveRepaintsAsked_;
        repaint(area.getSmallestIntegerContainer().expanded(2));
    }
    return moved;
}

void ParameterPage::addStateDrag(juce::Rectangle<float> area, std::string_view name, std::string key, double from,
                                 double span, double step, StateSet set, StateEdit reset, std::function<void()> click,
                                 double low, double high, bool wraps) {
    Region region;
    region.area = area;
    region.press = [this, from](juce::Point<float> p) {
        stateDragStart_ = from;
        stateDragLast_ = from;
        dragY_ = p.y;
    };
    region.drag = [this, label = std::string(name), dragKey = std::move(key), span, step, low, high, wraps,
                   apply = std::move(set)](juce::Point<float> p, bool fine) {
        const auto delta = static_cast<double>((dragY_ - p.y) / ctl::dragPixels * (fine ? ctl::fineDrag : 1.0f)) * span;
        const auto next = draggedValue(stateDragStart_, delta, step, low, high, wraps);
        if (juce::exactlyEqual(next, stateDragLast_)) return;
        stateDragLast_ = next;
        model_.applyEditDrag(label, dragKey, [apply, next](PluginState& s) { apply(s, next); });
    };
    region.release = [this] { model_.finishDrag(); };
    region.click = std::move(click);
    region.doubleClick = [this, label = std::string(name), toDefault = std::move(reset)] {
        model_.applyEdit(label, toDefault);
    };
    addRegion(std::move(region));
}

ParameterPage::FrameContent ParameterPage::paintTabbedFrame(juce::Graphics& g,
                                                            std::initializer_list<const char*> tabs) {
    return paintFrame(
        g, tabs, model_.panelTab(), [this](int index) { model_.showPanelTab(index); },
        [this](juce::Graphics& gg, float x) {
            SourceTab tab;
            if (model_.openSourceSlot() >= 0) {
                tab.name = sourceLabel(model_.modManifest(), model_.openSourceSlot());
                tab.open = model_.sourceTabShown();
                tab.select = [this] { model_.showSourceTab(); };
                tab.close = [this] { model_.closeSourceTab(); };
                tab.previous = [this] { model_.stepSourceTab(-1); };
                tab.next = [this] { model_.stepSourceTab(1); };
            }
            return paintSourceTab(gg, x, tab);
        });
}

void ParameterPage::onDefaultRestored(ParamId id) {
    int slot = 0, axis = 0;
    if (regionAngleOf(model_.manifest(), id, slot, axis)) model_.zeroRegionTurn(slot, axis);
}

float ParameterPage::paintSourceTab(juce::Graphics& g, float x, const SourceTab& tab) {
    if (!tab.select) return x;  // no source open: the bar is the three tabs and nothing else
    const auto tabFont = font(theme::type::body, theme::type::trackingTab);
    //  a source's settings: a temporary tab, and a cross back to the tab it replaced
    //  Step to the next source without going back to the matrix for it.
    const auto chevron = [&](juce::Rectangle<float> area, bool forward) {
        const auto mid = area.getCentreY();
        const auto near = forward ? area.getX() : area.getRight();
        const auto far = forward ? area.getRight() : area.getX();
        g.setColour(colour::label);
        g.drawLine(near, area.getY(), far, mid, theme::stroke::glyph);
        g.drawLine(far, mid, near, area.getBottom(), theme::stroke::glyph);
    };
    const juce::Rectangle<float> back{x, (ctl::tabBarHeight - ctl::stepGlyph) / 2.0f, ctl::stepGlyph, ctl::stepGlyph};
    chevron(back, false);
    stepAreas_[0] = back.expanded(ctl::stepGap / 2.0f);
    addRegion({stepAreas_[0], {}, {}, {}, tab.previous, {}});
    x += ctl::stepGlyph + ctl::stepGap;

    const juce::Rectangle<float> box{x, 0.0f, textWidth(tabFont, tab.name) + 2.0f * ctl::tabPadX, ctl::tabBarHeight};
    drawChoice(g, box, tab.name, tabFont, tab.open);
    addRegion({box, {}, {}, {}, tab.select, {}});

    const juce::Rectangle<float> forward{box.getRight() + ctl::stepGap, (ctl::tabBarHeight - ctl::stepGlyph) / 2.0f,
                                         ctl::stepGlyph, ctl::stepGlyph};
    chevron(forward, true);
    stepAreas_[1] = forward.expanded(ctl::stepGap / 2.0f);
    addRegion({stepAreas_[1], {}, {}, {}, tab.next, {}});

    const juce::Rectangle<float> cross{forward.getRight() + ctl::stepGap + ctl::closeGap,
                                       (ctl::tabBarHeight - ctl::closeGlyph) / 2.0f, ctl::closeGlyph, ctl::closeGlyph};
    addRegion({drawCloseCross(g, cross), {}, {}, {}, tab.close, {}});
    x = cross.getRight() + ctl::closeGap;
    return x;
}

}  // namespace bambi::ui
