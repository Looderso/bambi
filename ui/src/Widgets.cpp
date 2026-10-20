// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Widgets.h"

#include <algorithm>

#include "bambi/mod/matrix.hpp"
#include "bambi/mod/sources.hpp"
#include "bambi/ui/Draw.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;

float unitClamp(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

}  // namespace

void drawValueBar(juce::Graphics& g, juce::Rectangle<float> area, const BarContent& bar) {
    const auto x = area.getX();
    const auto w = area.getWidth();
    g.setColour(colour::track);
    g.fillRect(area);

    if (bar.tick) {
        //  Under the fill, so a value sitting exactly at zero still shows where zero is.
        g.setColour(colour::barTick);
        g.fillRect(x + w * unitClamp(bar.fillFrom) - theme::stroke::barTick / 2.0f, area.getY(), theme::stroke::barTick,
                   area.getHeight());
    }

    const auto from = unitClamp(std::min(bar.fillFrom, bar.fillTo));
    const auto to = unitClamp(std::max(bar.fillFrom, bar.fillTo));
    g.setColour(bar.enabled ? bar.ink : colour::inactive);
    g.fillRect(x + w * from, area.getY(), w * (to - from), area.getHeight());

    if (bar.modTo > bar.modFrom) {
        const auto m0 = unitClamp(bar.modFrom);
        const auto m1 = unitClamp(bar.modTo);
        g.setColour(colour::modulation());
        g.fillRect(x + w * m0, area.getY(), w * (m1 - m0), area.getHeight());
    }

    /*  Over everything: the fill is what you set, this is what the engine is applying -- a rate turning an
        angle, or modulation pushing a value. Keeping them as two marks is what lets the number stay honest
        about the parameter. */
    if (bar.hasLive) {
        //  it straddles the bar rather than sitting inside it, so it stays visible: a thin hairline
        //  confined inside the bar reads as invisible
        const juce::Rectangle<float> mark{x + w * unitClamp(bar.live) - theme::stroke::liveMark / 2.0f,
                                          area.getY() - ctl::liveMarkOver, theme::stroke::liveMark,
                                          area.getHeight() + 2.0f * ctl::liveMarkOver};
        //  disabled, it goes without the ring: two greys alone would not tell the states apart
        if (bar.enabled) {
            nowMark(g, mark, colour::liveValue);
        } else {
            g.setColour(colour::inactive);
            g.fillRect(mark);
        }
    }
}

void drawRemoteNotice(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label) {
    const auto line = theme::type::body * ctl::noticeLineHeight;
    const auto cy = area.getCentreY();
    text(g, label, font(theme::type::body), colour::text, {area.getX(), cy - 1.5f * line, area.getWidth(), line},
         juce::Justification::centred);
    text(g, "its controls have not arrived yet", labelFont(), colour::label,
         {area.getX(), cy - 0.5f * line, area.getWidth(), line}, juce::Justification::centred);
    text(g, "show this instance", labelFont(), colour::action, remoteNoticeLink(area), juce::Justification::centred);
}

juce::Rectangle<float> remoteNoticeLink(juce::Rectangle<float> area) {
    const auto line = theme::type::body * ctl::noticeLineHeight;
    return {area.getX(), area.getCentreY() + 0.5f * line, area.getWidth(), line};
}

juce::String fromCore(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

juce::String sourceLabel(const ModManifest& m, int slot) {
    const auto s = std::clamp(slot, 0, bambi::kNumSources - 1);
    const auto tab = static_cast<bambi::MatrixTab>(s / bambi::kSourcesPerTab);
    const auto name = fromCore(bambi::sourceColumn(m, tab, s % bambi::kSourcesPerTab).name);
    return tab == bambi::MatrixTab::Sidechain ? "sc " + name : name;
}

void drawChoice(juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& label, const juce::Font& f,
                bool chosen, bool offered) {
    if (chosen) {
        g.setColour(offered ? colour::chosen : colour::inactive);
        g.fillRect(cell);
    }
    text(g, label, f, chosen ? colour::textInverse : (offered ? colour::text : colour::inactive), cell,
         juce::Justification::centred);
}

std::vector<juce::Rectangle<float>> drawSegments(juce::Graphics& g, juce::Rectangle<float> area,
                                                 const std::vector<juce::String>& names, int selected, bool enabled,
                                                 int unavailable) {
    std::vector<juce::Rectangle<float>> cells;
    const auto cellWidth = area.getWidth() / static_cast<float>(std::max<std::size_t>(1, names.size()));
    for (std::size_t i = 0; i < names.size(); ++i) {
        const juce::Rectangle<float> cell{area.getX() + static_cast<float>(i) * cellWidth, area.getY(), cellWidth,
                                          area.getHeight()};
        const bool on = static_cast<int>(i) == selected;
        drawChoice(g, cell, names[i], font(theme::type::body), on,
                   enabled && (on || unavailable < 0 || static_cast<int>(i) < unavailable));
        cells.push_back(cell);
    }
    return cells;
}

float segmentsWidth(const std::vector<juce::String>& names) {
    float widest = 0.0f;
    for (const auto& n : names) widest = std::max(widest, framedButtonWidth(n));
    return widest * static_cast<float>(names.size());
}

float segmentLinesHeight(int count, int columns) {
    const int lines = (columns <= 0 || columns >= count) ? 1 : (count + columns - 1) / columns;
    return static_cast<float>(lines) * theme::controls::segmentHeight;
}

std::vector<juce::Rectangle<float>> drawSegmentLines(juce::Graphics& g, float x, float y, float width,
                                                     const std::vector<juce::String>& names, int selected, bool enabled,
                                                     int unavailable, int columns) {
    namespace ctl = theme::controls;
    const int count = static_cast<int>(names.size());
    if (columns <= 0 || columns >= count)
        return drawSegments(g, {x, y, width, ctl::segmentHeight}, names, selected, enabled, unavailable);
    std::vector<juce::Rectangle<float>> cells;
    const auto cellWidth = width / static_cast<float>(columns);
    for (int first = 0; first < count; first += columns) {
        const int n = std::min(columns, count - first);
        std::vector<juce::String> line(names.begin() + first, names.begin() + first + n);
        const int sel = (selected >= first && selected < first + n) ? selected - first : -1;
        const int un = unavailable < 0 || unavailable >= first + n ? -1 : std::max(0, unavailable - first);
        const auto lineCells =
            drawSegments(g, {x, y, cellWidth * static_cast<float>(n), ctl::segmentHeight}, line, sel, enabled, un);
        cells.insert(cells.end(), lineCells.begin(), lineCells.end());
        y += ctl::segmentHeight;
    }
    return cells;
}

void drawFramedButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, bool enabled,
                      bool on) {
    namespace colour = theme::colour;
    if (on) {
        g.setColour(colour::chosen);
        g.fillRect(area);
        text(g, label, font(theme::type::body), colour::textInverse, area, juce::Justification::centred);
        return;
    }
    g.setColour(enabled ? colour::text : colour::inactive);
    g.drawRect(area, theme::stroke::rule);
    text(g, label, font(theme::type::body), enabled ? colour::text : colour::inactive, area,
         juce::Justification::centred);
}

float framedButtonWidth(const juce::String& label) {
    return textWidth(font(theme::type::body), label) + 2.0f * theme::settings::buttonPadX;
}

float dropdownWidth() { return theme::controls::dropdownWidth + 2.0f * theme::controls::dropdownPad; }

void drawDropdown(juce::Graphics& g, juce::Rectangle<float> box, bool open, bool offered, float blockHeight) {
    namespace ct = theme::controls;
    if (open && offered) {
        g.setColour(theme::colour::chosen);
        g.fillRect(box.withSizeKeepingCentre(box.getWidth(), std::min(blockHeight, box.getHeight())));
    }
    juce::Path glyph;
    const auto c = box.getCentre();
    glyph.addTriangle(c.x - ct::dropdownWidth / 2.0f, c.y - ct::dropdownHeight / 2.0f, c.x + ct::dropdownWidth / 2.0f,
                      c.y - ct::dropdownHeight / 2.0f, c.x, c.y + ct::dropdownHeight / 2.0f);
    g.setColour(!offered ? theme::colour::inactive : open ? theme::colour::textInverse : theme::colour::label);
    g.fillPath(glyph);
}

void strokeChevron(juce::Graphics& g, juce::Rectangle<float> box, int dx, int dy, juce::Colour c) {
    namespace hdr = theme::header;
    const auto cx = box.getCentreX(), cy = box.getCentreY();
    const auto along = hdr::chevronW, across = hdr::chevronH;
    juce::Path p;
    if (dy == 0) {
        const auto d = dx > 0 ? along : -along;
        p.startNewSubPath(cx - d / 2.0f, cy - across);
        p.lineTo(cx + d / 2.0f, cy);
        p.lineTo(cx - d / 2.0f, cy + across);
    } else {
        const auto d = dy > 0 ? along : -along;
        p.startNewSubPath(cx - across, cy - d / 2.0f);
        p.lineTo(cx, cy + d / 2.0f);
        p.lineTo(cx + across, cy - d / 2.0f);
    }
    g.setColour(c);
    g.strokePath(p, juce::PathStrokeType(theme::stroke::glyph));
}

float drawGroupHeading(juce::Graphics& g, float x, float y, float width, const juce::String& title) {
    namespace ctl = theme::controls;
    text(g, title, labelFont(), theme::colour::label, {x, y, width, ctl::groupTitleHeight});
    ruleH(g, x, x + width, y + ctl::groupTitleHeight, theme::colour::rule);
    return y + ctl::groupTitleHeight + ctl::groupTitleGap;
}

void drawFactRow(juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, const juce::String& value,
                 bool set) {
    namespace colour = theme::colour;
    text(g, name, labelFont(), colour::label, row);
    if (value.isNotEmpty())
        text(g, value, font(theme::type::body), set ? colour::text : colour::inactive, row,
             juce::Justification::centredRight);
    ruleH(g, row.getX(), row.getRight(), row.getBottom() - theme::stroke::rule, colour::rowDivider);
}

void drawWaiting(juce::Graphics& g, juce::Rectangle<float> box, const juce::String& label, const juce::Font& f) {
    g.setColour(theme::colour::learning);
    g.fillRect(box);
    text(g, label, f, theme::colour::textInverse, box, juce::Justification::centred);
}

void drawTakes(juce::Graphics& g, juce::Rectangle<float> box) {
    juce::Path p;
    p.addRectangle(box.expanded(theme::controls::takesGap));
    g.setColour(theme::colour::learning);
    strokeDashed(g, p, theme::stroke::takesDash, juce::PathStrokeType(theme::stroke::rule));
}

void drawOutOfReach(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(theme::colour::ground.withAlpha(theme::controls::outOfReach));
    g.fillRect(box.expanded(theme::controls::takesGap + 1.0f));
}

float chipWidth(const juce::String& label, float height, bool accent, bool closable) {
    return textWidth(labelFont(), label) + 2.0f * static_cast<float>(theme::scene::presetChipPadX) +
           (accent ? theme::stroke::selectionBar : 0.0f) + (closable ? height : 0.0f);
}

Chip drawChip(juce::Graphics& g, juce::Point<float> topLeft, float height, const juce::String& label, juce::Colour ink,
              std::optional<juce::Colour> accent, bool closable) {
    Chip c;
    c.box = {topLeft.x, topLeft.y, chipWidth(label, height, accent.has_value(), closable), height};
    g.setColour(theme::colour::rule);
    g.drawRect(c.box, theme::stroke::rule);
    auto inner = c.box;
    if (accent) {
        g.setColour(*accent);
        g.fillRect(inner.removeFromLeft(theme::stroke::selectionBar));
    }
    if (closable) c.cross = drawCloseCross(g, inner.removeFromRight(height).reduced(height / 4.0f));
    text(g, label, labelFont(), ink, inner, juce::Justification::centred);
    return c;
}

juce::Rectangle<float> drawCloseCross(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(theme::colour::label);
    g.drawLine(box.getX(), box.getY(), box.getRight(), box.getBottom(), theme::stroke::glyph);
    g.drawLine(box.getRight(), box.getY(), box.getX(), box.getBottom(), theme::stroke::glyph);
    return box.expanded(theme::controls::closeGap / 2.0f);
}

void drawPlayGlyph(juce::Graphics& g, juce::Rectangle<float> box, bool playing) {
    juce::Path triangle;
    triangle.addTriangle(box.getX(), box.getY(), box.getRight(), box.getCentreY(), box.getX(), box.getBottom());
    g.setColour(playing ? theme::colour::playhead : theme::colour::text);
    if (playing) g.fillPath(triangle);
    g.strokePath(triangle, juce::PathStrokeType(theme::stroke::glyph));
}

void drawStopGlyph(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(theme::colour::text);
    g.drawRect(box, theme::stroke::glyph);
}

void drawPlayMark(juce::Graphics& g, juce::Rectangle<float> box, bool plays, bool onBlock) {
    namespace colour = theme::colour;
    g.setColour(colour::textInverse);
    g.fillRect(box);
    g.setColour(onBlock ? colour::textInverse : colour::text);
    g.drawRect(box, theme::stroke::toggle);
    if (plays) {
        g.setColour(colour::text);
        g.fillRect(box.reduced(theme::header::toggleWell));
    }
}

void paintMenuFrame(juce::Graphics& g, juce::Rectangle<float> box) {
    g.setColour(theme::colour::ground);
    g.fillRect(box);
    g.setColour(theme::colour::ruleStrong);
    g.drawRect(box, theme::stroke::rule);
}

Menu drawMenu(juce::Graphics& g, juce::Point<float> topLeft, float width, const std::vector<MenuItem>& items,
              const juce::String& foot) {
    namespace colour = theme::colour;
    namespace sh = theme::sheet;
    const float footHeight = foot.isEmpty() ? 0.0f : sh::menuLine;
    Menu menu;
    menu.box = {topLeft.x, topLeft.y, width,
                2.0f * sh::menuPad + sh::menuLine * static_cast<float>(items.size()) + footHeight};
    paintMenuFrame(g, menu.box);
    auto inner = menu.box.reduced(sh::menuPad, sh::menuPad);
    for (const MenuItem& item : items) {
        const auto row = inner.removeFromTop(sh::menuLine);
        const auto ink = item.offered ? colour::text : colour::inactive;
        text(g, item.label, labelFont(), ink, row, juce::Justification::centredLeft);
        text(g, item.note, smallFont(), item.offered ? colour::label : colour::inactive, row,
             juce::Justification::centredRight);
        menu.items.push_back(row);
    }
    if (!foot.isEmpty()) {
        g.setColour(colour::rule);
        g.drawHorizontalLine(static_cast<int>(inner.getY()), inner.getX(), inner.getRight());
        text(g, foot, smallFont(), colour::label, inner.removeFromTop(footHeight), juce::Justification::centredLeft);
    }
    return menu;
}

juce::Rectangle<float> drawPageBand(juce::Graphics& g, float width) {
    const auto h = static_cast<float>(theme::metrics::headerHeight);
    g.setColour(theme::colour::ground);
    g.fillRect(0.0f, 0.0f, width, h);
    g.setColour(theme::colour::rule);
    g.fillRect(0.0f, h - theme::stroke::rule, width, theme::stroke::rule);
    return {theme::metrics::bandInset, 0.0f, width - 2.0f * theme::metrics::bandInset, h};
}

float sceneToggleWidth(const juce::String& label) {
    return theme::scene::toggleBox + theme::scene::toggleGap + textWidth(labelFont(), label);
}

SceneToggle paintSceneToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge,
                             const juce::String& label, bool on, bool enabled) {
    namespace colour = theme::colour;
    namespace sc = theme::scene;

    const auto font = labelFont();
    const auto wide = textWidth(font, label);
    const auto labelX = rightEdge - wide;
    const juce::Rectangle<float> box{labelX - sc::toggleGap - sc::toggleBox,
                                     strip.getY() + (strip.getHeight() - sc::toggleBox) / 2.0f, sc::toggleBox,
                                     sc::toggleBox};

    //  filled when on, outlined when off; grey only when it cannot be used
    if (on) {
        g.setColour(enabled ? colour::chosen : colour::inactive);
        g.fillRect(box);
    } else {
        g.setColour(enabled ? colour::text : colour::inactive);
        g.drawRect(box, theme::stroke::toggle);
    }
    //  greyed, never hidden: what is not in use here is inert, and says so
    text(g, label, font, enabled ? colour::label : colour::inactive,
         {labelX, strip.getY(), wide + 1.0f, strip.getHeight()});

    //  the hit area covers the square and the label: a label you cannot click looks broken
    const auto left = box.getX() - sc::toggleHitPad;
    return {{left, strip.getY(), rightEdge - left + sc::toggleHitPad, strip.getHeight()}, left};
}

juce::Rectangle<float> sceneFrame(const Viewport& vp) {
    return {static_cast<float>(vp.cx - vp.rx), static_cast<float>(vp.cy - vp.ry), static_cast<float>(2.0 * vp.rx),
            static_cast<float>(2.0 * vp.ry)};
}

void paintSceneFrame(juce::Graphics& g, const Viewport& vp, Projection projection) {
    g.setColour(theme::colour::sphere);
    if (projection == Projection::Globe)
        g.drawEllipse(sceneFrame(vp), theme::stroke::rule);
    else
        g.drawRect(sceneFrame(vp), theme::stroke::rule);
}

Viewport sceneViewport(Projection projection, int width, int height) {
    const auto strip = static_cast<double>(theme::scene::labelStrip);
    const auto w = static_cast<double>(width);
    const auto h = static_cast<double>(height) - strip;
    Viewport vp;
    vp.cx = w / 2.0;
    vp.cy = strip + h / 2.0;
    if (projection == Projection::Globe) {
        vp.rx = vp.ry = static_cast<double>(theme::scene::globeRadius) * std::min(w, h) / 2.0;
        return vp;
    }
    /*  Room above the frame for `b l f r b`, which sit outside it: without it a picture that reaches
        the top of its area puts them in the label strip, on top of the corner toggles. The letters
        are centred a gap above the frame in a box one strip tall, so half of that is what they need. */
    const auto room = static_cast<double>(theme::scene::axisLabelGap) + strip / 2.0;
    const auto usable = std::max(1.0, h - room);
    const auto wide = std::min(static_cast<double>(theme::scene::equirectWidth) * w,
                               2.0 * static_cast<double>(theme::scene::equirectHeight) * usable);
    vp.rx = wide / 2.0;
    vp.ry = vp.rx / 2.0;
    vp.cy = strip + room + usable / 2.0;
    return vp;
}

SceneRow sceneRow(int left, int top, bool full) {
    namespace m = theme::metrics;
    const auto panel = m::scenePanel;
    const auto gap = theme::space::gap;
    if (full) return {{}, {left, top, panel + gap + panel, panel}};
    return {{left, top, panel, panel}, {left + panel + gap, top, panel, panel}};
}

std::optional<float> drawNiceGrid(juce::Graphics& g, juce::Rectangle<float> plot, double polarity, int maxTicks) {
    namespace ctl = theme::controls;
    std::optional<float> zeroRow;
    for (const auto& tick : polarityAxis(polarity, maxTicks)) {
        //  the tick is already in the curve's own [0, 1]: converting it again would misplace the zero
        const auto row = plot.getBottom() - plot.getHeight() * static_cast<float>(tick.at);
        ruleH(g, plot.getX(), plot.getRight(), row, tick.zero ? theme::colour::rule : theme::colour::envelopeGrid);
        text(g, juce::String(axisTickLabel(tick.value)), labelFont(),
             tick.zero ? theme::colour::text : theme::colour::label,
             {plot.getX() - ctl::envelopeGutter, row - ctl::envelopeGutter / 2.0f,
              ctl::envelopeGutter - ctl::envelopeLabelGap, ctl::envelopeGutter},
             juce::Justification::centredRight);
        if (tick.zero) zeroRow = row;
    }
    return zeroRow;
}

}  // namespace bambi::ui
