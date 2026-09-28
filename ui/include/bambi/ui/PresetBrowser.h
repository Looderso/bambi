// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <utility>
#include <vector>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/PresetModel.h"
#include "bambi/ui/TextField.h"

/*  The preset browser, the same in every plugin. It opens from the header's preset name over the
 *  panel, as a source's page does, so the scene and the matrix stay in sight while presets are tried.
 *
 *  A search field that has the keyboard the moment it opens; the list -- factory, then user, folders
 *  folded or not -- where ONE CLICK LOADS, because a load is one undo step; and under it save,
 *  save as, rename and delete. A name is typed in one row that both save as and rename use, and a
 *  slash in it is the folder. What the list holds, its order and what a search finds are `core`'s
 *  (`presetRows`); this draws them and takes the mouse and the keys.
 */
namespace bambi::ui {

class PresetBrowser final : public HitArea {
public:
    /// Ask a question over the whole window, and run `confirmed` on a yes: the window's, since it covers it.
    using Ask = std::function<void(juce::String question, juce::String note, juce::String yes,
                                   std::function<void()> confirmed)>;

    /*  `changed` runs after anything here changed the patch or its preset's name: the window has
        controls to bring up to date. `close` hides the browser -- the window's too, since the header
        shows whether it is open. */
    PresetBrowser(PresetModel& model, std::function<void()> changed, std::function<void()> close, Ask ask);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress& key) override { return handleKey(key); }

    /// Once a frame: repaint when what it lists, or which preset is current or modified, moved under it.
    void refresh();
    /// Put the cursor on the current preset, as opening does: the header's arrows stepped while this was open.
    void followCurrent();
    /// The field has the keyboard again: a question over the window was answered.
    void takeKeyboard();

    //  ---- for checks: what a person does, where it was drawn --------------------------------
    enum class Action { Save, SaveAs, Rename, Delete };
    std::vector<PresetRow> rowsShown() const { return rows_; }
    juce::Rectangle<float> rowArea(const PresetRef& ref) const;
    juce::Rectangle<float> headingArea(const std::string& heading) const;
    juce::Rectangle<float> actionArea(Action a) const { return actions_[static_cast<std::size_t>(a)]; }
    bool actionEnabled(Action a) const { return enabled_[static_cast<std::size_t>(a)]; }
    juce::Rectangle<float> closeArea() const { return close_; }
    void typeSearch(const juce::String& text);
    /// Type into the name row and commit it, as return does. False when the row is not open.
    bool typeName(const juce::String& text);
    bool naming() const { return naming_ != Naming::None; }
    juce::String nameOffered() const { return entry_.getText(); }
    juce::String noteShown() const { return note_; }
    bool pressKey(const juce::KeyPress& key) { return handleKey(key); }
    PresetRef cursor() const;

private:
    enum class Naming { None, SaveAs, Rename };

    bool handleKey(const juce::KeyPress& key);
    void moveCursor(int delta);
    void load(const PresetRef& ref);
    void startNaming(Naming what);
    void commitName();
    void stopNaming();
    std::vector<PresetRef> listed() const;
    juce::String signature();

    PresetModel& model_;
    std::function<void()> changed_, closeNow_;
    Ask ask_;
    TextField search_, entry_;
    Naming naming_{Naming::None};
    juce::String note_;  ///< why a name was refused, under the row it was typed in
    int cursor_{0};      ///< among the presets listed
    juce::String shown_;

    std::vector<PresetRow> rows_;
    std::vector<std::pair<PresetRef, juce::Rectangle<float>>> rowAreas_;
    std::vector<std::pair<std::string, juce::Rectangle<float>>> headingAreas_;
    std::array<juce::Rectangle<float>, 4> actions_{};
    std::array<bool, 4> enabled_{};
    juce::Rectangle<float> close_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowser)
};

}  // namespace bambi::ui
