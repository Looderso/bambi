// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/PresetBrowser.h"

#include <algorithm>

#include "bambi/ui/Draw.h"
#include "bambi/ui/Theme.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {
namespace {
namespace colour = theme::colour;
namespace ps = theme::presets;
namespace ctl = theme::controls;
namespace st = theme::settings;

constexpr const char* kActionLabels[] = {"save", "save as", "rename", "delete"};

}  // namespace

PresetBrowser::PresetBrowser(PresetModel& model, std::function<void()> changed, std::function<void()> close, Ask ask)
    : model_(model),
      changed_(std::move(changed)),
      closeNow_(std::move(close)),
      ask_(std::move(ask)),
      search_(ps::pathLength),
      entry_(ps::pathLength) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setVisible(false);

    search_.setSelectAllWhenFocused(false);
    search_.setTextToShowWhenEmpty("search", colour::inactive);
    search_.onTextChange = [this] {
        cursor_ = 0;
        repaint();
    };
    search_.onKey = [this](const juce::KeyPress& key) { return handleKey(key); };
    addAndMakeVisible(search_);

    entry_.setTextToShowWhenEmpty("name, or folder/name", colour::inactive);
    entry_.onReturnKey = [this] { commitName(); };
    entry_.onEscapeKey = [this] { stopNaming(); };
    addChildComponent(entry_);
}

void PresetBrowser::visibilityChanged() {
    if (!isVisible()) {
        //  the keyboard goes back to the host: the next thing after choosing a preset is play
        if (search_.hasKeyboardFocus(false)) search_.giveAwayKeyboardFocus();
        return;
    }
    //  opened: what is on the disk now, no search, no name half typed, the cursor on what is loaded
    model_.refresh();
    search_.setText({}, false);
    stopNaming();
    followCurrent();
    if (search_.isShowing()) search_.grabKeyboardFocus();
}

std::vector<PresetRef> PresetBrowser::listed() const {
    std::vector<PresetRef> out;
    for (const auto& r : rows_)
        if (r.kind == PresetRow::Kind::Preset) out.push_back(r.ref);
    return out;
}

PresetRef PresetBrowser::cursor() const {
    const auto shown = listed();
    return cursor_ >= 0 && cursor_ < static_cast<int>(shown.size()) ? shown[static_cast<std::size_t>(cursor_)]
                                                                    : PresetRef{};
}

void PresetBrowser::followCurrent() {
    //  what is loaded is shown: its folder and its group unfold
    const auto current = model_.current();
    if (!current.none()) {
        model_.setFolded(presetHeading(current.factory), false);
        if (!current.folder.empty()) model_.setFolded(presetHeading(current.factory, current.folder), false);
    }
    rows_ = presetRows(model_.all(), search_.getText().toStdString(),
                       [this](std::string_view h) { return model_.folded(h); });
    const auto shown = listed();
    const auto at = std::find(shown.begin(), shown.end(), current);
    cursor_ = at == shown.end() ? 0 : static_cast<int>(at - shown.begin());
    repaint();
}

juce::String PresetBrowser::signature() {
    juce::String s;
    for (const auto& ref : model_.all()) s << (ref.factory ? "f" : "u") << juce::String(presetPath(ref)) << "|";
    s << juce::String(presetPath(model_.current())) << (model_.modified() ? "*" : "")
      << (model_.available() ? "" : "-");
    return s;
}

void PresetBrowser::refresh() {
    if (!isVisible()) return;
    if (!model_.available()) {
        if (closeNow_) closeNow_();  // the window turned to another instance: these presets are not its
        return;
    }
    const auto now = signature();
    if (now == shown_) return;
    shown_ = now;
    repaint();
}

// ---- the keys: the list is walked while the typing stays in the field ------------------------

bool PresetBrowser::handleKey(const juce::KeyPress& key) {
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::upKey) {
        moveCursor(key == juce::KeyPress::downKey ? 1 : -1);
        return true;
    }
    if (key == juce::KeyPress::returnKey) {
        if (const auto ref = cursor(); !ref.none()) load(ref);
        return true;
    }
    if (key == juce::KeyPress::escapeKey) {
        //  the first one clears a search, the second closes
        if (search_.getText().isNotEmpty()) {
            search_.setText({}, false);
            followCurrent();
        } else if (closeNow_)
            closeNow_();
        return true;
    }
    return false;
}

void PresetBrowser::moveCursor(int delta) {
    const auto count = static_cast<int>(listed().size());
    if (count == 0) return;
    cursor_ = std::clamp(cursor_ + delta, 0, count - 1);
    scrollIntoView(rowArea(cursor()));
    repaint();
}

void PresetBrowser::load(const PresetRef& ref) {
    if (!model_.load(ref)) return;
    const auto shown = listed();
    if (const auto at = std::find(shown.begin(), shown.end(), ref); at != shown.end())
        cursor_ = static_cast<int>(at - shown.begin());
    if (changed_) changed_();
    repaint();
}

// ---- a name typed: save as, and rename ---------------------------------------------------------

void PresetBrowser::startNaming(Naming what) {
    naming_ = what;
    note_.clear();
    const auto current = model_.current();
    juce::String offered;
    if (what == Naming::Rename)
        offered = juce::String(presetPath(current));
    else if (!current.none() && !(current == kDefaultPreset))
        offered = (current.factory || current.folder.empty() ? juce::String() : juce::String(current.folder) + "/") +
                  juce::String(current.name) + " 2";
    entry_.setText(offered, false);
    entry_.setVisible(true);
    resized();
    if (entry_.isShowing()) {
        entry_.grabKeyboardFocus();
        //  the name and not its folder: that is the part about to be typed over
        entry_.setHighlightedRegion({offered.indexOfChar('/') + 1, offered.length()});
    }
    repaint();
}

void PresetBrowser::stopNaming() {
    naming_ = Naming::None;
    note_.clear();
    entry_.setVisible(false);
    resized();
    if (isShowing()) search_.grabKeyboardFocus();
    repaint();
}

void PresetBrowser::commitName() {
    const auto ref = parsePresetPath(entry_.getText().toStdString());
    if (!ref.has_value()) {
        note_ = "a preset needs a name";
        repaint();
        return;
    }
    const auto& all = model_.all();
    const bool exists = std::find(all.begin(), all.end(), *ref) != all.end();
    const auto done = [this] {
        stopNaming();
        followCurrent();
        if (changed_) changed_();
    };

    if (naming_ == Naming::Rename) {
        if (*ref == model_.current()) return stopNaming();
        if (exists || !model_.rename(model_.current(), *ref)) {
            note_ = exists ? "that name is taken" : "could not be renamed";
            repaint();
            return;
        }
        return done();
    }

    const auto write = [this, ref = *ref, done] {
        if (model_.save(ref)) return done();
        note_ = "could not be written";
        repaint();
    };
    //  over another preset asks first; over the one that is loaded is what "save" does anyway
    if (exists && !(*ref == model_.current()) && ask_)
        ask_("replace \"" + juce::String(presetPath(*ref)) + "\"?", "the preset saved under that name is lost.",
             "replace", write);
    else
        write();
}

void PresetBrowser::takeKeyboard() {
    if (isShowing()) (naming_ == Naming::None ? search_ : entry_).grabKeyboardFocus();
}

void PresetBrowser::typeSearch(const juce::String& text) {
    search_.setText(text, false);
    cursor_ = 0;
    repaint();
}

bool PresetBrowser::typeName(const juce::String& text) {
    if (naming_ == Naming::None) return false;
    entry_.setText(text, false);
    commitName();
    return true;
}

// ---- where things were drawn -------------------------------------------------------------------

juce::Rectangle<float> PresetBrowser::rowArea(const PresetRef& ref) const {
    for (const auto& [r, area] : rowAreas_)
        if (r == ref) return area;
    return {};
}

juce::Rectangle<float> PresetBrowser::headingArea(const std::string& heading) const {
    for (const auto& [h, area] : headingAreas_)
        if (h == heading) return area;
    return {};
}

void PresetBrowser::resized() {
    const auto inset = static_cast<float>(theme::space::inset);
    const auto width = static_cast<float>(getWidth());
    const auto fieldY = (st::titleHeight - ctl::segmentHeight) / 2.0f;
    search_.setBounds(juce::Rectangle<float>(inset, fieldY, width - 3.0f * inset - ctl::closeGlyph, ctl::segmentHeight)
                          .getSmallestIntegerContainer());

    //  the name row sits over the actions; its two buttons are drawn, the field is a child
    const auto buttons = framedButtonWidth(naming_ == Naming::Rename ? "rename" : "save") +
                         framedButtonWidth("cancel") + 2.0f * st::buttonGap;
    const auto folders = naming_ != Naming::None && !model_.userFolders().empty();
    const auto rowTop =
        static_cast<float>(getHeight()) - 2.0f * st::titleHeight - (folders ? ps::chipHeight + ps::chipGap : 0.0f);
    entry_.setBounds(juce::Rectangle<float>(inset, rowTop + fieldY, width - 2.0f * inset - buttons, ctl::segmentHeight)
                         .getSmallestIntegerContainer());
}

void PresetBrowser::paint(juce::Graphics& g) {
    clearRegions();
    rowAreas_.clear();
    headingAreas_.clear();
    g.fillAll(colour::ground);

    const auto inset = static_cast<float>(theme::space::inset);
    const auto width = static_cast<float>(getWidth());
    const auto height = static_cast<float>(getHeight());
    const bool searching = search_.getText().trim().isNotEmpty();
    const auto current = model_.current();
    const bool modified = model_.modified();

    rows_ = presetRows(model_.all(), search_.getText().toStdString(),
                       [this](std::string_view h) { return model_.folded(h); });
    const auto shown = listed();
    cursor_ = std::clamp(cursor_, 0, std::max(0, static_cast<int>(shown.size()) - 1));
    const auto cursorRef = cursor();

    //  ---- what stays put under the list: a name being typed, and the four actions -------------
    const auto folders = model_.userFolders();
    const bool chips = naming_ != Naming::None && !folders.empty();
    const auto namingHeight =
        naming_ == Naming::None ? 0.0f : st::titleHeight + (chips ? ps::chipHeight + ps::chipGap : 0.0f);
    const auto listBottom = height - st::titleHeight - namingHeight;

    //  ---- the list, scrolling between the two bars ---------------------------------------------
    setViewport(st::titleHeight, listBottom);
    {
        const ContentClip clip(*this, g);
        auto y = st::titleHeight - scroll();
        if (rows_.empty())
            text(g, "nothing matches", labelFont(), colour::label, {inset, y, width - 2.0f * inset, ps::groupHeight});
        for (const auto& row : rows_) {
            if (row.kind == PresetRow::Kind::Group) {
                const juce::Rectangle<float> area{0.0f, y, width, ps::groupHeight};
                if (!searching) {
                    strokeChevron(g, {inset, area.getY(), ps::foldBox, area.getHeight()}, row.folded ? 1 : 0,
                                  row.folded ? 0 : 1, colour::label);
                    headingAreas_.emplace_back(row.heading, area);
                    addClickArea(area, [this, key = row.heading, folded = row.folded] {
                        model_.setFolded(key, !folded);
                        repaint();
                    });
                }
                text(g, row.ref.factory ? "factory" : "user", labelFont(), colour::label,
                     area.withTrimmedLeft(inset + (searching ? 0.0f : ps::foldBox)));
                ruleH(g, inset, width - inset, area.getBottom() - theme::stroke::rule, colour::rule);
                y += ps::groupHeight;
                continue;
            }
            if (row.kind == PresetRow::Kind::Folder) {
                const juce::Rectangle<float> area{0.0f, y, width, ctl::listRowHeight};
                const auto left = inset + ps::indent;
                strokeChevron(g, {left, area.getY(), ps::foldBox, area.getHeight()}, row.folded ? 1 : 0,
                              row.folded ? 0 : 1, colour::label);
                text(g, juce::String(row.ref.folder), font(theme::type::body), colour::label,
                     area.withTrimmedLeft(left + ps::foldBox));
                if (row.folded)
                    text(g, juce::String(row.count), labelFont(), colour::inactive, area.withTrimmedRight(inset),
                         juce::Justification::centredRight);
                headingAreas_.emplace_back(row.heading, area);
                addClickArea(area, [this, key = row.heading, folded = row.folded] {
                    model_.setFolded(key, !folded);
                    repaint();
                });
                y += ctl::listRowHeight;
                continue;
            }

            const juce::Rectangle<float> area{0.0f, y, width, ctl::listRowHeight};
            const bool isCurrent = row.ref == current;
            if (isCurrent) {
                g.setColour(colour::chosen);
                g.fillRect(area);
            }
            if (row.ref == cursorRef && !isCurrent) {
                g.setColour(colour::highlight);
                g.fillRect(area);
            }
            const auto left = inset + ps::indent + (row.inFolder ? ps::indent + ps::foldBox : 0.0f);
            juce::String tag;
            if (isCurrent && modified) tag = "modified";
            if (!row.tag.empty())
                tag = tag.isEmpty() ? juce::String(row.tag)
                                    : tag + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(row.tag);
            const auto tagWidth = tag.isEmpty() ? 0.0f : textWidth(labelFont(), tag) + st::buttonGap;
            text(g, juce::String(row.ref.name), font(theme::type::body), isCurrent ? colour::textInverse : colour::text,
                 area.withTrimmedLeft(left).withTrimmedRight(inset + tagWidth));
            if (tag.isNotEmpty())
                text(g, tag, labelFont(), isCurrent ? colour::inactive : colour::label, area.withTrimmedRight(inset),
                     juce::Justification::centredRight);
            rowAreas_.emplace_back(row.ref, area);
            addClickArea(area, [this, ref = row.ref] { load(ref); });
            y += ctl::listRowHeight;
        }
        noteContentBottom(y + scroll());
    }

    //  ---- the search row: the field is a child; the cross closes ------------------------------
    g.setColour(colour::ground);
    g.fillRect(0.0f, 0.0f, width, st::titleHeight);
    close_ = drawCloseCross(g, {width - inset - ctl::closeGlyph, (st::titleHeight - ctl::closeGlyph) / 2.0f,
                                ctl::closeGlyph, ctl::closeGlyph});
    addClickArea(close_.expanded(ctl::closeGlyph), [this] {
        if (closeNow_) closeNow_();
    });
    ruleH(g, 0.0f, width, st::titleHeight - theme::stroke::rule, colour::ruleStrong);

    //  ---- the name row ---------------------------------------------------------------------------
    if (naming_ != Naming::None) {
        const auto top = listBottom;
        g.setColour(colour::ground);
        g.fillRect(0.0f, top, width, namingHeight);
        ruleH(g, 0.0f, width, top, colour::rule);
        const auto fieldY = top + (st::titleHeight - ctl::segmentHeight) / 2.0f;
        const juce::String commit = naming_ == Naming::Rename ? "rename" : "save";
        //  cancel on the left, the commit on the right, as the question's two are
        juce::Rectangle<float> ok{width - inset - framedButtonWidth(commit), fieldY, framedButtonWidth(commit),
                                  ctl::segmentHeight};
        juce::Rectangle<float> cancel{ok.getX() - st::buttonGap - framedButtonWidth("cancel"), fieldY,
                                      framedButtonWidth("cancel"), ctl::segmentHeight};
        drawFramedButton(g, ok, commit, true);
        drawFramedButton(g, cancel, "cancel", true);
        addClickArea(ok, [this] { commitName(); });
        addClickArea(cancel, [this] { stopNaming(); });

        if (chips) {
            //  the folders there are, a click away: nobody has to type the slash
            auto x = inset;
            const auto chipY = top + st::titleHeight - ps::chipGap;
            const auto chip = [&](const juce::String& label, const std::string& folder) {
                const juce::Rectangle<float> box{
                    x, chipY, textWidth(labelFont(), label) + 2.0f * static_cast<float>(theme::scene::presetChipPadX),
                    ps::chipHeight};
                if (box.getRight() > width - inset) return;
                g.setColour(colour::rule);
                g.drawRect(box, theme::stroke::rule);
                text(g, label, labelFont(), colour::label, box, juce::Justification::centred);
                addClickArea(box, [this, folder] {
                    const auto typed = parsePresetPath(entry_.getText().toStdString());
                    const auto name = typed.has_value() ? juce::String(typed->name) : juce::String();
                    entry_.setText(folder.empty() ? name : juce::String(folder) + "/" + name, false);
                    entry_.grabKeyboardFocus();
                });
                x = box.getRight() + ps::chipGap;
            };
            for (const auto& folder : folders) chip(juce::String(folder) + "/", folder);
            chip("no folder", {});
        }
        if (note_.isNotEmpty())
            text(g, note_, labelFont(), colour::label,
                 {inset, top - ctl::listRowHeight, width - 2.0f * inset, ctl::listRowHeight});
    }

    //  ---- the actions ----------------------------------------------------------------------------
    const auto barTop = height - st::titleHeight;
    g.setColour(colour::ground);
    g.fillRect(0.0f, barTop, width, st::titleHeight);
    ruleH(g, 0.0f, width, barTop, colour::ruleStrong);
    const bool user = !current.none() && !current.factory;
    //  greyed while a name is being typed: two "save"s at once would be two things
    const bool idle = naming_ == Naming::None;
    enabled_ = {idle && user && modified, idle, idle && user, idle && user};
    auto x = inset;
    for (int i = 0; i < 4; ++i) {
        const auto at = static_cast<std::size_t>(i);
        const juce::String label = kActionLabels[i];
        actions_[at] = {x, barTop + (st::titleHeight - ctl::segmentHeight) / 2.0f, framedButtonWidth(label),
                        ctl::segmentHeight};
        drawFramedButton(g, actions_[at], label, enabled_[at]);
        x = actions_[at].getRight() + st::buttonGap;
        if (!enabled_[at]) continue;
        addClickArea(actions_[at], [this, i] {
            const auto now = model_.current();
            switch (static_cast<Action>(i)) {
                case Action::Save:
                    if (model_.save(now) && changed_) changed_();
                    repaint();
                    break;
                case Action::SaveAs: startNaming(Naming::SaveAs); break;
                case Action::Rename: startNaming(Naming::Rename); break;
                case Action::Delete:
                    if (ask_)
                        ask_("delete \"" + juce::String(presetPath(now)) + "\"?",
                             "it goes to the Trash. the sound stays as it is.", "delete", [this, now] {
                                 model_.remove(now);
                                 followCurrent();
                                 if (changed_) changed_();
                             });
                    break;
            }
        });
    }
}

}  // namespace bambi::ui
