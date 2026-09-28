// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/SettingsPage.h"

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace ctl = theme::controls;
namespace st = theme::settings;
}  // namespace

SettingsPage::SettingsPage(juce::String plugin, std::function<SettingsContent()> content,
                           std::function<void(const juce::String&)> rename, std::function<void()> close)
    : plugin_(std::move(plugin)),
      content_(std::move(content)),
      rename_(std::move(rename)),
      closeNow_(std::move(close)),
      name_(st::nameLength) {
    setOpaque(true);

    //  return and escape give the keyboard back: a field that kept it would swallow the host's space bar,
    //  and the next thing a person does after naming a track is press play
    name_.onReturnKey = [this] {
        commitName();
        name_.giveAwayKeyboardFocus();
    };
    name_.onFocusLost = [this] { commitName(); };
    name_.onEscapeKey = [this] {
        name_.setText(content_ ? content_().name : juce::String(), false);
        name_.giveAwayKeyboardFocus();
    };
    addAndMakeVisible(name_);
}

void SettingsPage::commitName() {
    //  held to what the bus's label carries, in bytes and on a whole character: the field's own
    //  restriction counts characters and only what is typed, so a pasted or accented name could
    //  still be cut mid-character on its way to another window
    auto name = name_.getText().trim();
    while (name.getNumBytesAsUTF8() > static_cast<size_t>(st::nameLength)) name = name.dropLastCharacters(1);
    if (name != name_.getText()) name_.setText(name, false);
    if (rename_) rename_(name);
    repaint();
}

void SettingsPage::typeName(const juce::String& text) {
    name_.setText(text, false);
    commitName();
}

void SettingsPage::open(bool show, juce::Component& footer) {
    setVisible(show);
    if (!show) return;
    toFront(false);
    footer.toFront(false);  // what is always on screen stays on screen
}

/*  What the page shows that can change under it -- the session's peers, the transport, a choice
    another window or an undo moved -- as one line to compare. */
void SettingsPage::refresh() {
    if (!isVisible() || !content_) return;
    const auto c = content_();
    juce::String now;
    now << (c.linked ? 1 : 0) << c.session << c.peers << c.order << c.trackName;
    for (const auto& choice : c.own) now << choice.selected << (choice.enabled ? 1 : 0);
    for (const auto& [name, value] : c.readout) now << value;
    if (now == shown_) return;
    shown_ = now;
    ++refreshes_;
    repaint();
}

void SettingsPage::visibilityChanged() {
    //  opened: the field says what the name is now, not what it was when the page was last closed
    if (isVisible() && content_) {
        const auto c = content_();
        name_.setText(c.name, false);
        name_.setTextToShowWhenEmpty(c.trackName.isEmpty() ? juce::String("the track's name") : c.trackName,
                                     colour::inactive);
    }
}

juce::Rectangle<float> SettingsPage::optionArea(int choice, int option) const {
    if (choice < 0 || choice >= static_cast<int>(options_.size())) return {};
    const auto& row = options_[static_cast<std::size_t>(choice)];
    return option >= 0 && option < static_cast<int>(row.size()) ? row[static_cast<std::size_t>(option)]
                                                                : juce::Rectangle<float>{};
}

void SettingsPage::resized() {
    const auto inset = static_cast<float>(theme::space::inset);
    const auto top = st::titleHeight + ctl::contentTop + ctl::groupTitleHeight + ctl::groupTitleGap;
    const auto half = (static_cast<float>(getWidth()) - 2.0f * inset - st::columnGap) / 2.0f;
    //  in the "name" row of the instance group: the row's right-hand end
    const juce::Rectangle<float> field{inset + half - st::fieldWidth, top + (st::rowHeight - ctl::segmentHeight) / 2.0f,
                                       st::fieldWidth, ctl::segmentHeight};
    name_.setBounds(field.getSmallestIntegerContainer());
}

void SettingsPage::paint(juce::Graphics& g) {
    clearRegions();
    options_.clear();
    actions_.clear();
    g.fillAll(colour::ground);
    const auto c = content_ ? content_() : SettingsContent{};

    const auto inset = static_cast<float>(theme::space::inset);
    const auto width = static_cast<float>(getWidth());
    const auto half = (width - 2.0f * inset - st::columnGap) / 2.0f;
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");

    //  ---- the title, and the way back ---------------------------------------------------------
    text(g, plugin_ + dot + "settings", font(theme::type::body, theme::type::trackingTab), colour::text,
         {inset, 0.0f, width - 2.0f * inset, st::titleHeight});
    close_ = drawCloseCross(g, {width - inset - ctl::closeGlyph, (st::titleHeight - ctl::closeGlyph) / 2.0f,
                                ctl::closeGlyph, ctl::closeGlyph});
    addClickArea(close_, [this] {
        if (closeNow_) closeNow_();
    });
    ruleH(g, 0.0f, width, st::titleHeight, colour::ruleStrong);

    const auto title = [&](float x, float y, const juce::String& name) {
        return drawGroupHeading(g, x, y, half, name);
    };
    const auto row = [&](float x, float y, const juce::String& name, const juce::String& value, bool set) {
        drawFactRow(g, {x, y, half, st::rowHeight}, name, value, set);
        return y + st::rowHeight;
    };

    //  ---- the shared frame: what every instance of every plugin has ---------------------------
    auto y = title(inset, st::titleHeight + ctl::contentTop, "instance");
    y = row(inset, y, "name", {}, true);  // the field is a child, laid out over this row's right end
    y += static_cast<float>(theme::space::groupGap);

    y = title(inset, y, "session");
    //  the session as the header names it: its first four characters, which is what a person compares
    y = row(inset, y, "link",
            c.linked ? "session " + c.session.substring(0, 4) + dot + juce::String(c.peers) +
                           (c.peers == 1 ? " instance" : " instances")
                     : juce::String("not joined"),
            c.linked);
    y += static_cast<float>(theme::space::groupGap);

    y = title(inset, y, "output");
    row(inset, y, "order", c.order >= 0 ? juce::String(c.order) + dot + "from the track's channels" : juce::String("-"),
        c.order >= 0);

    //  ---- the plugin's own ---------------------------------------------------------------------
    const auto x2 = inset + half + st::columnGap;
    auto y2 = st::titleHeight + ctl::contentTop;
    if (!c.own.empty() || !c.actions.empty()) {
        y2 = title(x2, y2, plugin_);
        for (const auto& choice : c.own) {
            text(g, choice.label, labelFont(), colour::label, {x2, y2, half, ctl::groupTitleHeight});
            y2 += ctl::groupTitleHeight;
            //  segments, as every choice of a few is drawn
            auto cells =
                drawSegments(g, {x2, y2, half, ctl::segmentHeight}, choice.options, choice.selected, choice.enabled);
            for (int i = 0; i < static_cast<int>(cells.size()); ++i)
                if (choice.enabled && i != choice.selected && choice.choose)
                    addClickArea(cells[static_cast<std::size_t>(i)], [choose = choice.choose, i, this] {
                        choose(i);
                        repaint();
                    });
            options_.push_back(std::move(cells));
            y2 += ctl::segmentHeight + ctl::segmentGap;
            if (choice.note.isNotEmpty()) {
                const auto line = theme::type::label * ctl::noticeLineHeight;
                text(g, choice.note, labelFont(), colour::label, {x2, y2, half, line});
                y2 += line;
            }
            y2 += ctl::sectionGap;
        }
        for (const auto& action : c.actions) {
            const juce::Rectangle<float> r{x2, y2, half, st::rowHeight};
            text(g, action.label, labelFont(), colour::label, r);
            const auto wide = framedButtonWidth(action.button);
            const juce::Rectangle<float> button{
                r.getRight() - wide, r.getY() + (st::rowHeight - ctl::segmentHeight) / 2.0f, wide, ctl::segmentHeight};
            drawFramedButton(g, button, action.button, true);
            actions_.push_back(button);
            if (action.run) addClickArea(button, action.run);
            y2 += st::rowHeight;
        }
        y2 += static_cast<float>(theme::space::groupGap);
    }
    if (!c.readout.empty()) {
        y2 = title(x2, y2, "host");
        for (const auto& [name, value] : c.readout) y2 = row(x2, y2, name, value, true);
    }
}

}  // namespace bambi::ui
