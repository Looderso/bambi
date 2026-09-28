// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/Header.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace colour = theme::colour;
namespace hdr = theme::header;

namespace {
//  pictograms: 0 / 45 / 90 degree strokes, one weight, no fills
juce::Path undoGlyph() {
    juce::Path p;
    p.startNewSubPath(12.0f, 4.0f);
    p.lineTo(4.0f, 4.0f);
    p.startNewSubPath(6.0f, 1.5f);
    p.lineTo(3.5f, 4.0f);
    p.lineTo(6.0f, 6.5f);
    return p;
}

juce::Path settingsGlyph() {
    juce::Path p;
    p.addEllipse(4.6f, 4.6f, 4.8f, 4.8f);
    const std::array<std::array<float, 4>, 8> spokes{{{7, 1, 7, 3},
                                                      {7, 11, 7, 13},
                                                      {1, 7, 3, 7},
                                                      {11, 7, 13, 7},
                                                      {2.8f, 2.8f, 4.2f, 4.2f},
                                                      {9.8f, 9.8f, 11.2f, 11.2f},
                                                      {11.2f, 2.8f, 9.8f, 4.2f},
                                                      {4.2f, 9.8f, 2.8f, 11.2f}}};
    for (const auto& s : spokes) {
        p.startNewSubPath(s[0], s[1]);
        p.lineTo(s[2], s[3]);
    }
    return p;
}
}  // namespace

Header::Header(InstanceModel& instances, juce::String plugin, std::function<void()> onSettings,
               std::function<void()> onUndo, std::function<void()> onRedo)
    : instances_(instances),
      plugin_(std::move(plugin)),
      onSettings_(std::move(onSettings)),
      onUndo_(std::move(onUndo)),
      onRedo_(std::move(onRedo)) {}

void Header::layoutTabs(juce::Rectangle<float> middle) {
    tabs_.clear();
    tabArea_ = middle;
    tabMaxScroll_ = 0.0f;
    prevChevron_ = nextChevron_ = {};
    const auto n = static_cast<int>(static_cast<std::size_t>(instances_.instanceCount()));

    //  Recorded even when there is no room to draw them, so refresh() can still tell that the set changed.
    paintedCount_ = static_cast<std::size_t>(instances_.instanceCount());
    const bool selectionMoved = !(instances_.selectedInstance() == lastSelected_);
    lastSelected_ = instances_.selectedInstance();
    if (n == 0 || middle.getWidth() < hdr::tabMinWidth) return;

    const auto ideal = (middle.getWidth() - static_cast<float>(n - 1) * hdr::tabGap) / static_cast<float>(n);
    const auto w = std::clamp(ideal, hdr::tabMinWidth, hdr::tabMaxWidth);
    const auto stride = w + hdr::tabGap;

    /*  Chevrons cost width, so they only appear once the tabs actually overflow -- and by then every tab is
        already at hdr::tabMinWidth, so taking the width back cannot shrink them further. Both ends are reserved
        together: one appearing and disappearing as you scroll would shuffle every tab sideways. */
    if (static_cast<float>(n) * stride - hdr::tabGap > middle.getWidth() + 0.5f) {
        prevChevron_ = {middle.getX(), middle.getY(), hdr::chevronBox, middle.getHeight()};
        nextChevron_ = {middle.getRight() - hdr::chevronBox, middle.getY(), hdr::chevronBox, middle.getHeight()};
        middle = middle.withTrimmedLeft(hdr::chevronBox).withTrimmedRight(hdr::chevronBox);
        tabArea_ = middle;
    }
    tabMaxScroll_ = std::max(0.0f, static_cast<float>(n) * stride - hdr::tabGap - middle.getWidth());

    /*  A selection made anywhere else -- the scene, the instance closing, another window -- brings itself
        into view. A scroll the user made is left alone, or the two would fight every frame. */
    int index = -1;
    for (int i = 0; i < n; ++i)
        if (instances_.instanceAt(static_cast<int>(i)) == instances_.selectedInstance()) index = i;
    if (index >= 0 && selectionMoved) {
        const auto at = static_cast<float>(index) * stride;
        if (at < tabScroll_)
            tabScroll_ = at;
        else if (at + w > tabScroll_ + middle.getWidth())
            tabScroll_ = at + w - middle.getWidth();
    }
    tabScroll_ = std::clamp(tabScroll_, 0.0f, tabMaxScroll_);

    for (int i = 0; i < n; ++i) {
        tabs_.push_back(
            {instances_.instanceAt(i),
             instances_.instanceLabel(i),
             {middle.getX() + static_cast<float>(i) * stride - tabScroll_, middle.getY(), w, middle.getHeight()}});
    }
}

void Header::setSettingsOpen(bool open) {
    if (open == settingsOpen_) return;
    settingsOpen_ = open;
    repaint();
}

void Header::setUndoState(bool canUndo, bool canRedo, juce::String undoName, juce::String redoName) {
    undoName_ = std::move(undoName);  // a tooltip only: no repaint
    redoName_ = std::move(redoName);
    if (canUndo == canUndo_ && canRedo == canRedo_) return;
    canUndo_ = canUndo;
    canRedo_ = canRedo;
    repaint();
}

juce::String Header::getTooltip() {
    const auto at = getMouseXYRelative().toFloat();
    const auto grab = static_cast<float>(theme::space::grid) / 2.0f;
    const auto tip = [](const char* verb, const juce::String& name) {
        return name.isEmpty() ? juce::String(verb) : juce::String(verb) + " " + name;
    };
    if (canUndo_ && undoBounds_.expanded(grab).contains(at)) return tip("undo", undoName_);
    if (canRedo_ && redoBounds_.expanded(grab).contains(at)) return tip("redo", redoName_);
    return {};
}

void Header::setStatus(Link status) {
    const bool same = status.open == status_.open && status.session == status_.session && status.peers == status_.peers;
    status_ = status;
    if (!same) repaint();
}

juce::Rectangle<float> Header::settingsBounds() const {
    const auto h = static_cast<float>(getHeight());
    return {static_cast<float>(getWidth()) - hdr::inset - hdr::glyphBox, (h - hdr::glyphBox) / 2.0f, hdr::glyphBox,
            hdr::glyphBox};
}

juce::Rectangle<float> Header::iconBounds() const {
    const auto h = static_cast<float>(getHeight());
    return {hdr::inset, (h - hdr::iconBox) / 2.0f, hdr::iconBox, hdr::iconBox};
}

/*  Each mark reuses a shape its own plugin already draws elsewhere -- the encoder's sphere and
    source, Reverb's fading rings, Echo's skewed, decaying repeat -- so the icon reads as this
    plugin rather than as separate branding. Every dot clears the rim by the same margin the
    encoder's source dot does, so no bead sits inside the rim's stroke and reads as outside the
    sphere rather than on it. */
void Header::paintIcon(juce::Graphics& g) const {
    const auto box = iconBounds();
    const auto cx = box.getX() + 8.0f;
    const auto cy = box.getY() + 8.0f;
    const auto ring = [&](float r, float alpha, float strokeW) {
        g.setColour(colour::sphere.withAlpha(alpha));
        g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, strokeW);
    };
    const auto dot = [&](float dx, float dy, float r, juce::Colour c, float alpha) {
        g.setColour(c.withAlpha(alpha));
        g.fillEllipse(cx + dx - r, cy + dy - r, r * 2.0f, r * 2.0f);
    };

    if (plugin_ == "encoder") {
        ring(6.0f, 1.0f, theme::stroke::glyph);
        dot(2.5f, -2.0f, 1.2f, colour::source, 1.0f);
    } else if (plugin_ == "echo") {
        ring(6.0f, 1.0f, theme::stroke::glyph);
        juce::Path trace;
        trace.startNewSubPath(3.27f, 1.64f);
        trace.cubicTo(2.86f, -0.82f, 1.53f, -2.75f, 0.0f, -4.09f);
        g.setColour(colour::sphere.withAlpha(0.5f));
        g.strokePath(trace, juce::PathStrokeType(theme::stroke::graticule), juce::AffineTransform::translation(cx, cy));
        dot(3.27f, 1.64f, 1.09f, colour::source, 1.0f);
        dot(2.59f, -0.74f, 0.82f, colour::sphere, 0.6f);
        dot(1.53f, -2.75f, 0.6f, colour::sphere, 0.35f);
        dot(0.0f, -4.09f, 0.44f, colour::sphere, 0.18f);
    } else if (plugin_ == "reverb") {
        dot(0.0f, 0.0f, 1.4f, colour::source, 1.0f);
        ring(3.8f, 0.75f, theme::stroke::graticule);
        ring(6.4f, 0.4f, theme::stroke::graticule);
    }
}

void Header::paint(juce::Graphics& g) {
    const auto h = static_cast<float>(getHeight());
    const auto cy = h / 2.0f;
    auto x = hdr::inset;

    paintIcon(g);
    x += hdr::iconBox + hdr::gap;

    const auto wordmark = wordmarkFont();
    //  The beta stands in for the name's first letter; every other glyph is plain lower-case.
    const juce::String wordmarkText = juce::String::fromUTF8("\xce\xb2") + "ambi";
    text(g, wordmarkText, wordmark, colour::text, {x, 0.0f, textWidth(wordmark, wordmarkText) + 2.0f, h});
    x += textWidth(wordmark, wordmarkText) + hdr::gap;
    g.setColour(colour::rule);
    g.fillRect(x, cy - hdr::dividerHeight / 2.0f, theme::stroke::rule, hdr::dividerHeight);
    x += theme::stroke::rule + hdr::gap;
    //  reserved at "encoder"'s width, the longest word in the suite -- every other plugin's is
    //  shorter, so this is the one word that never moves what follows it, and the header does not
    //  jump when an instance switches product
    const auto productWidth = textWidth(labelFont(), "encoder");
    text(g, plugin_, labelFont(), colour::label, {x, 0.0f, productWidth + 2.0f, h});
    homeArea_ = {hdr::inset, 0.0f, x + productWidth + 2.0f - hdr::inset, h};  // icon, wordmark and name
    x += productWidth + hdr::gap;
    g.setColour(colour::rule);
    g.fillRect(x, cy - hdr::dividerHeight / 2.0f, theme::stroke::rule, hdr::dividerHeight);
    x += theme::stroke::rule + hdr::gap;

    //  Undo and redo: the editor's history of state edits -- matrix, trajectory, triggers. Host
    //  parameters are the host's to undo.
    const juce::PathStrokeType glyphStroke(theme::stroke::glyph);
    const auto undo = undoGlyph();
    undoBounds_ = {x, cy - hdr::glyphBox / 2.0f, hdr::glyphBox, hdr::glyphBox};
    g.setColour(canUndo_ ? colour::text : colour::inactive);
    g.strokePath(undo, glyphStroke, juce::AffineTransform::translation(x, cy - hdr::glyphBox / 2.0f + 2.0f));
    x += hdr::glyphBox + hdr::gap / 2.0f;
    redoBounds_ = {x, cy - hdr::glyphBox / 2.0f, hdr::glyphBox, hdr::glyphBox};
    g.setColour(canRedo_ ? colour::text : colour::inactive);
    g.strokePath(
        undo, glyphStroke,
        juce::AffineTransform::scale(-1.0f, 1.0f).translated(x + hdr::glyphBox, cy - hdr::glyphBox / 2.0f + 6.0f));

    //  from the right: settings, link, preset
    //  settings and the preset browser are toggles, and say they are open as an open tab does
    const auto openBlock = [&](float left, float right) {
        g.setColour(colour::chosen);
        g.fillRect(left - hdr::togglePadX, hdr::tabTop, right - left + 2.0f * hdr::togglePadX,
                   h - theme::stroke::identityRule - hdr::tabTop);
    };
    const auto gear = settingsBounds();
    if (settingsOpen_) openBlock(gear.getX(), gear.getRight());
    g.setColour(settingsOpen_ ? colour::textInverse : colour::text);
    g.strokePath(settingsGlyph(), glyphStroke, juce::AffineTransform::translation(gear.getX(), gear.getY()));
    auto right = gear.getX() - hdr::gap;

    const auto linkText = status_.open ? "link " + juce::String::fromUTF8("\xc2\xb7") + " session " +
                                             status_.session.substring(0, 4) + " " +
                                             juce::String::fromUTF8("\xc2\xb7") + " " + juce::String(status_.peers)
                                       : "link " + juce::String::fromUTF8("\xc2\xb7") + " not joined";
    const auto linkWidth = textWidth(labelFont(), linkText) + 2.0f;
    right -= linkWidth;
    text(g, linkText, labelFont(), colour::label, {right, 0.0f, linkWidth, h});
    right -= hdr::gap / 2.0f + hdr::statusSquare;
    g.setColour(status_.open ? colour::ok : colour::inactive);
    g.fillRect(right, cy - hdr::statusSquare / 2.0f, hdr::statusSquare, hdr::statusSquare);
    right -= hdr::gap;

    //  `< preset . name* >`: the chevrons step, the name opens the browser
    const auto live = preset_.available;
    presetNext_ = {right - hdr::presetStep, cy - hdr::presetStep / 2.0f, hdr::presetStep, hdr::presetStep};
    strokeChevron(g, presetNext_, 1, 0, live ? colour::text : colour::inactive);
    right = presetNext_.getX() - hdr::gap / 4.0f;

    const auto presetName =
        (preset_.name.isEmpty() ? juce::String("-") : preset_.name) + (preset_.modified ? " *" : "");
    const auto presetLabel = "preset " + juce::String::fromUTF8("\xc2\xb7") + " ";
    const auto nameWidth = std::min(hdr::presetName, textWidth(labelFont(), presetName) + 2.0f);
    const auto presetWidth = textWidth(labelFont(), presetLabel);
    presetName_ = {right - nameWidth - presetWidth, 0.0f, presetWidth + nameWidth, h};
    if (preset_.open) openBlock(presetName_.getX(), presetName_.getRight());
    const auto ink = [&](juce::Colour c) { return !live ? colour::inactive : preset_.open ? colour::textInverse : c; };
    right -= nameWidth;
    text(g, presetName, labelFont(), ink(colour::text), {right, 0.0f, nameWidth, h});
    right -= presetWidth;
    text(g, presetLabel, labelFont(), ink(colour::label), {right, 0.0f, presetWidth, h});
    right -= hdr::gap / 4.0f + hdr::presetStep;
    presetPrev_ = {right, cy - hdr::presetStep / 2.0f, hdr::presetStep, hdr::presetStep};
    strokeChevron(g, presetPrev_, -1, 0, live ? colour::text : colour::inactive);

    //  The instances fill what is left between the two clusters, whatever the link status text costs.
    const auto tabsLeft = x + hdr::gap;
    layoutTabs({tabsLeft, hdr::tabTop, std::max(0.0f, right - hdr::gap - tabsLeft), h - hdr::tabTop});

    if (!tabs_.empty()) {
        juce::Graphics::ScopedSaveState clipped(g);
        g.reduceClipRegion(tabArea_.getSmallestIntegerContainer());
        for (const auto& tab : tabs_) {
            //  the open one stands on the plugin's rule, which runs on beneath it unbroken
            const auto block = tab.area.withBottom(h - theme::stroke::identityRule);
            const bool on = tab.id == instances_.selectedInstance();
            if (on) {
                g.setColour(colour::chosen);
                g.fillRect(block);
            }
            text(g, tab.label, labelFont(), on ? colour::textInverse : colour::text, block.reduced(hdr::tabPadX, 0.0f),
                 juce::Justification::centred);
        }
    }

    //  Chevrons: shown only when there is something past that end, so they never promise a move they
    //  cannot make. Clicking one pages the strip; the wheel stays as the shortcut.
    if (tabMaxScroll_ > 0.0f) {
        strokeChevron(g, prevChevron_, -1, 0, tabScroll_ > 0.5f ? colour::text : colour::inactive);
        strokeChevron(g, nextChevron_, 1, 0, tabScroll_ < tabMaxScroll_ - 0.5f ? colour::text : colour::inactive);
    }

    //  the plugin's rule under the header, in its own colour, the whole width and unbroken
    g.setColour(identity_);
    g.fillRect(0.0f, h - theme::stroke::identityRule, static_cast<float>(getWidth()), theme::stroke::identityRule);
}

void Header::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    //  Only when they overflow: a wheel that moves nothing reads as a broken control.
    if (tabMaxScroll_ <= 0.0f || !tabArea_.contains(e.position)) return;
    const auto d = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    const auto next = std::clamp(tabScroll_ - d * hdr::tabWheel, 0.0f, tabMaxScroll_);
    if (next != tabScroll_) {
        tabScroll_ = next;
        repaint();
    }
}

void Header::setPreset(Preset preset) {
    if (preset == preset_) return;
    preset_ = std::move(preset);
    repaint();
}

void Header::onPreset(std::function<void()> toggleBrowser, std::function<void(int)> step) {
    onPresetName_ = std::move(toggleBrowser);
    onPresetStep_ = std::move(step);
}

bool Header::refresh() {
    //  compared against what was painted, not against a guess at what might have changed. Nothing
    //  else repaints this component
    bool same = (lastSelected_ == instances_.selectedInstance()) &&
                paintedCount_ == static_cast<std::size_t>(instances_.instanceCount());
    for (std::size_t i = 0; same && i < tabs_.size() && i < static_cast<std::size_t>(instances_.instanceCount()); ++i)
        same = tabs_[i].id == instances_.instanceAt(static_cast<int>(i)) &&
               tabs_[i].label == instances_.instanceLabel(static_cast<int>(i));
    if (!same) {
        ++asked_;
        repaint();
    }
    return !same;
}

void Header::mouseDown(const juce::MouseEvent& e) {
    if (tabMaxScroll_ > 0.0f) {
        const auto page = std::max(hdr::tabMinWidth, tabArea_.getWidth());
        if (prevChevron_.contains(e.position) || nextChevron_.contains(e.position)) {
            const auto by = nextChevron_.contains(e.position) ? page : -page;
            const auto next = std::clamp(tabScroll_ + by, 0.0f, tabMaxScroll_);
            if (next != tabScroll_) {
                tabScroll_ = next;
                repaint();
            }
            return;
        }
    }

    if (homeArea_.contains(e.position) && onHome_) return onHome_();

    if (tabArea_.contains(e.position))
        for (const auto& tab : tabs_)
            if (tab.area.contains(e.position)) {
                instances_.selectInstance(tab.id);
                return;
            }

    const auto grab = static_cast<float>(theme::space::grid) / 2.0f;
    if (preset_.available) {
        const bool back = presetPrev_.expanded(grab / 2.0f).contains(e.position);
        if ((back || presetNext_.expanded(grab / 2.0f).contains(e.position)) && onPresetStep_)
            return onPresetStep_(back ? -1 : 1);
        if (presetName_.contains(e.position) && onPresetName_) return onPresetName_();
    }
    if (settingsBounds().expanded(grab).contains(e.position) && onSettings_)
        onSettings_();
    else if (canUndo_ && undoBounds_.expanded(grab).contains(e.position) && onUndo_)
        onUndo_();
    else if (canRedo_ && redoBounds_.expanded(grab).contains(e.position) && onRedo_)
        onRedo_();
}

}  // namespace bambi::ui
