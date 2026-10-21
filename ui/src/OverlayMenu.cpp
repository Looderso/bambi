// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/OverlayMenu.h"

#include <algorithm>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

namespace {
namespace colour = theme::colour;
namespace sc = theme::scene;
namespace sh = theme::sheet;

}  // namespace

OverlayToggleAreas paintOverlayToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge,
                                      const juce::String& label, const OverlaySelection& selection, bool listed,
                                      const OverlayMenuState& state) {
    const float glyphBox = dropdownWidth();
    OverlayToggleAreas areas;
    areas.triangle = {rightEdge - glyphBox, strip.getY(), glyphBox, strip.getHeight()};

    //  the toggle sits left of its triangle, close enough to read as one control with it
    const auto toggle = paintSceneToggle(g, strip, areas.triangle.getX(), label, selection.master(), listed);
    areas.toggle = toggle.hit;
    areas.left = toggle.left;

    drawDropdown(g, areas.triangle, state.open, listed, sc::toggleBox + 2.0f);
    return areas;
}

OverlayMenuAreas paintOverlayMenu(juce::Graphics& g, float rightEdge, float top, std::span<const OverlayRow> rows,
                                  const OverlaySelection& selection, const OverlayMenuState& state) {
    OverlayMenuAreas areas;
    areas.rows.assign(rows.size(), {});
    if (!state.open || rows.empty()) return areas;

    const float line = sh::menuLine;
    float widest = 0.0f;
    for (const auto& row : rows) {
        const float indent = row.part ? sh::overlayPartIndent : theme::shape::otherSourceDot * 2.0f + sh::overlayRowGap;
        widest = std::max(widest, indent + textWidth(labelFont(), row.label));
    }
    const float width =
        std::max(sh::overlayMinWidth, 2.0f * sh::overlayRowPad + sc::toggleBox + sh::overlayRowGap + widest);
    const int lines = static_cast<int>(rows.size()) + 1;  // all and none above the rows
    const float height = 2.0f * sh::menuPad + line * static_cast<float>(std::min(lines, sh::overlayMaxRows));
    areas.box = {std::max(0.0f, rightEdge - width), top, width, height};
    areas.maxScroll = std::max(0.0f, line * static_cast<float>(lines - 1) - (height - 2.0f * sh::menuPad - line));

    paintMenuFrame(g, areas.box);

    auto inner = areas.box.reduced(0.0f, sh::menuPad);
    //  all and none stay put above the rows, which scroll under them
    const auto head = inner.removeFromTop(line).reduced(sh::overlayRowPad, 0.0f);
    const float allWidth = textWidth(labelFont(), "all"), noneWidth = textWidth(labelFont(), "none");
    areas.all = {head.getX(), head.getY(), allWidth, head.getHeight()};
    areas.none = {areas.all.getRight() + 2.0f * sh::overlayRowGap, head.getY(), noneWidth, head.getHeight()};
    text(g, "all", labelFont(), colour::label, areas.all, juce::Justification::centredLeft);
    text(g, "none", labelFont(), colour::label, areas.none, juce::Justification::centredLeft);
    g.setColour(colour::rule);
    g.drawHorizontalLine(static_cast<int>(inner.getY()), areas.box.getX(), areas.box.getRight());

    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(inner.getSmallestIntegerContainer());
    const bool dim = !selection.master();
    float y = inner.getY() - state.scroll;
    for (std::size_t i = 0; i < rows.size(); ++i, y += line) {
        const juce::Rectangle<float> box{areas.box.getX(), y, areas.box.getWidth(), line};
        if (box.getBottom() <= inner.getY() || box.getY() >= inner.getBottom()) continue;
        areas.rows[i] = box.getIntersection(inner);
        const auto& row = rows[i];
        if (state.hovered.has_value() && *state.hovered == row.key) {
            g.setColour(colour::highlight);
            g.fillRect(box);
        }
        float x = box.getX() + sh::overlayRowPad + (row.part ? sh::overlayPartIndent - sc::toggleBox : 0.0f);
        const juce::Rectangle<float> tick{x, box.getCentreY() - sc::toggleBox / 2.0f, sc::toggleBox, sc::toggleBox};
        drawToggleBox(g, tick, selection.chosen(row.key), !dim);
        x = tick.getRight() + sh::overlayRowGap;
        if (!row.part) {
            const float r = theme::shape::otherSourceDot;
            g.setColour(dim ? row.swatch.withAlpha(sc::backAlpha) : row.swatch);
            g.fillEllipse(x, box.getCentreY() - r, 2.0f * r, 2.0f * r);
            x += 2.0f * r + sh::overlayRowGap;
        }
        text(g, row.label, labelFont(),
             dim        ? colour::inactive
             : row.part ? colour::label
                        : colour::text,
             {x, box.getY(), box.getRight() - sh::overlayRowPad - x, box.getHeight()},
             juce::Justification::centredLeft);
    }
    return areas;
}

bool overlayHover(const OverlayMenuAreas& areas, std::span<const OverlayRow> rows, OverlayMenuState& state,
                  juce::Point<float> at) {
    const auto over = state.open ? overlayRowAt(areas, rows, at) : std::nullopt;
    if (over == state.hovered) return false;
    state.hovered = over;
    return true;
}

namespace {

/// The pointer left the view. True when something had been hovered.
bool overlayClearHover(OverlayMenuState& state) {
    if (!state.hovered.has_value()) return false;
    state.hovered.reset();
    return true;
}

}  // namespace

std::optional<OverlayKey> overlayRowAt(const OverlayMenuAreas& areas, std::span<const OverlayRow> rows,
                                       juce::Point<float> at) {
    for (std::size_t i = 0; i < rows.size() && i < areas.rows.size(); ++i)
        if (areas.rows[i].contains(at)) return rows[i].key;
    return std::nullopt;
}

void addOverlayClicks(HitArea& on, const OverlayToggleAreas& toggle, const OverlayMenuAreas& menu,
                      std::span<const OverlayRow> rows, OverlaySelection& selection, OverlayMenuState& state,
                      juce::Rectangle<float> whole, std::function<void()> changed) {
    const bool listed = !rows.empty();
    if (state.open && !listed) state.open = false;  // everything left the bus under an open list

    if (state.open) {
        //  a press anywhere else closes it, there and then, and goes no further: it never orbits the globe it was over, and a
        //  drag begun there turns nothing -- the next one does
        on.addDragArea(whole,
                       [&state, changed](juce::Point<float>) {
                           state.open = false;
                           state.hovered.reset();
                           changed();
                       },
                       {}, {});
        //  the list's own area takes the wheel and any press that is not on a row, and does nothing with it. First, so the
        //  rows lie on top: the first area under a press takes it, whatever it does.
        on.addWheelArea(menu.box, [&state, max = menu.maxScroll, changed](float by) {
            const float next = std::clamp(state.scroll + by, 0.0f, max);
            if (!juce::exactlyEqual(next, state.scroll)) {
                state.scroll = next;
                changed();
            }
        });
        std::vector<OverlayKey> keys;
        for (const auto& row : rows) keys.push_back(row.key);
        on.addClickArea(menu.all, [&selection, keys, changed] {
            selection.chooseAll(keys, true);
            changed();
        });
        on.addClickArea(menu.none, [&selection, keys, changed] {
            selection.chooseAll(keys, false);
            changed();
        });
        for (std::size_t i = 0; i < rows.size() && i < menu.rows.size(); ++i) {
            if (menu.rows[i].isEmpty()) continue;
            const OverlayKey key = rows[i].key;
            on.addClickArea(menu.rows[i], [&selection, key, changed] {
                selection.choose(key, !selection.chosen(key));
                changed();
            });
        }
    }
    //  the strip's own two, over the closing area: a second press on the triangle closes, the toggle still toggles
    if (!listed) return;
    on.addClickArea(toggle.toggle, [&selection, changed] {
        selection.setMaster(!selection.master());
        changed();
    });
    on.addClickArea(toggle.triangle, [&state, changed] {
        state.open = !state.open;
        state.scroll = 0.0f;
        state.hovered.reset();
        changed();
    });
}

void OverlayMenu::setRows(std::vector<OverlayRow> rows) {
    rows_ = std::move(rows);
    std::vector<OverlayKey> present;
    present.reserve(rows_.size());
    for (const auto& row : rows_) present.push_back(row.key);
    selection_.forget(present);
    if (state_.open && rows_.empty()) state_.open = false;  // everything left the bus under an open list
}

float OverlayMenu::paintToggle(juce::Graphics& g, juce::Rectangle<float> strip, float rightEdge) {
    right_ = rightEdge;
    toggle_ = paintOverlayToggle(g, strip, rightEdge, label_, selection_, !rows_.empty(), state_);
    return toggle_.left;
}

void OverlayMenu::paintList(juce::Graphics& g, float top) {
    list_ = paintOverlayMenu(g, right_, top, rows_, selection_, state_);
}

void OverlayMenu::addClicks(HitArea& on, juce::Rectangle<float> whole, std::function<void()> changed) {
    //  Escape reaches the view only while it has the keyboard: opening the list takes it
    on.setWantsKeyboardFocus(true);
    addOverlayClicks(on, toggle_, list_, rows_, selection_, state_, whole, [&on, this, changed] {
        if (state_.open) on.grabKeyboardFocus();
        changed();
    });
}

bool OverlayMenu::mouseMove(juce::Point<float> at) { return overlayHover(list_, rows_, state_, at); }

bool OverlayMenu::mouseExit() { return overlayClearHover(state_); }

bool OverlayMenu::keyPressed(const juce::KeyPress& key) {
    if (!state_.open || key != juce::KeyPress::escapeKey) return false;
    state_.open = false;
    state_.hovered.reset();
    return true;
}

}  // namespace bambi::ui
