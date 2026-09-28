// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "bambi/editor/PluginEditor.h"
#include "bambi/host/PluginProcessor.h"

/*  The preset checks, written once and run by every plugin's check suite: presets are the shared
 *  processor's and the shared window's, so what is checked is the same in all three, and a copy in
 *  each suite would be three to keep in step. Everything is driven the way a person drives it --
 *  the header's name and chevrons, the browser's rows and buttons, the question's two answers --
 *  in a directory of the check's own (`BAMBI_PRESET_DIR`), so nothing of the user's is read or
 *  written.
 */
namespace bambi::editor {

struct PresetCheckSpec {
    std::string moved;   ///< a float parameter's key, to move and watch: any that a preset carries
    std::string kept{};  ///< a key a preset leaves alone, where the plugin has one
};

template <class Processor>
void checkPresets(const PresetCheckSpec& spec, const std::function<void(bool, const juce::String&)>& check) {
    namespace fs = std::filesystem;
    using Action = ui::PresetBrowser::Action;

    const auto scratch = fs::temp_directory_path() / "bambi-check-presets";
    fs::remove_all(scratch);
    host::PluginProcessor::usePresetDirectory(scratch);

    Processor proc;
    host::PluginProcessor& base = proc;
    const auto& m = base.linkHost().manifest();
    const auto root = host::PluginProcessor::presetRoot(base.linkHost().product());
    check(root.string().starts_with(scratch.string()), "presets: a check's directory is its own, not the user's");

    const int at = m.byKey(spec.moved);
    auto* param = base.hostParameter(static_cast<ParamId>(at));
    if (at == kNoParam || param == nullptr) return check(false, "presets: the check names a parameter this plugin has");
    const auto plain = [&] { return param->convertFrom0to1(param->getValue()); };
    const auto setPlain = [&](float v) { param->setValueNotifyingHost(param->convertTo0to1(v)); };
    const float home = plain();
    const float away = home + 0.37f * (m[at].max - m[at].min) <= m[at].max ? home + 0.37f * (m[at].max - m[at].min)
                                                                           : home - 0.37f * (m[at].max - m[at].min);
    const auto near = [&](float a, float b) { return std::abs(a - b) <= 1.0e-3f * (m[at].max - m[at].min); };

    std::unique_ptr<juce::AudioProcessorEditor> window(proc.createEditor());
    auto* editor = dynamic_cast<PluginEditor*>(window.get());
    if (editor == nullptr) return check(false, "presets: the plugin's window is the shared one");
    const auto frame = [&] {
        editor->tick();
        editor->paintNow();
    };
    auto& browser = editor->presetBrowser();

    //  ---- a fresh instance is the default preset, and moving anything says so ------------------
    frame();
    check(editor->presetShown() == "default", "presets: a fresh instance shows 'default', unmodified");
    setPlain(away);
    frame();
    check(editor->presetShown() == "default *", "presets: a parameter moved marks the preset modified");

    //  ---- the name opens the browser over the panel; save as writes a file, in a folder ---------
    check(editor->clickPresetName() && editor->presetsOpen(), "presets: the header's name opens the browser");
    frame();
    check(!browser.actionEnabled(Action::Save) && !browser.actionEnabled(Action::Rename) &&
              !browser.actionEnabled(Action::Delete) && browser.actionEnabled(Action::SaveAs),
          "presets: on a factory preset only 'save as' is live");
    check(editor->clickPresetAction(Action::SaveAs) && browser.naming(), "presets: 'save as' opens the name row");
    frame();  // what is live is decided while painting
    check(!browser.actionEnabled(Action::SaveAs) && !browser.actionEnabled(Action::Save),
          "presets: the actions are greyed while a name is typed");
    check(browser.typeName("checks/one") && !browser.naming(), "presets: a name with a slash is taken");
    frame();
    const PresetRef one{false, "checks", "one"};
    check(fs::exists(root / "checks" / "one.json"),
          "presets: the file is <folder>/<name>.json in the plugin's directory");
    check(editor->presetShown() == "one", "presets: the saved preset is the current one, unmodified");
    check(!browser.rowArea(one).isEmpty() && !browser.headingArea("user/checks").isEmpty(),
          "presets: it is listed under its folder's heading");

    //  ---- save: live only on a modified user preset, and it clears the mark -------------------
    check(!browser.actionEnabled(Action::Save), "presets: 'save' is greyed while nothing has moved");
    base.document().edit(
        "a cell", [&](PluginState& s) { s.matrix.push_back({MatrixTab::Features, 0, static_cast<ParamId>(at), 0.5}); });
    frame();
    check(editor->presetShown() == "one *", "presets: a matrix cell added marks it modified, not only a parameter");
    check(editor->clickPresetAction(Action::Save), "presets: 'save' is live on a modified user preset");
    frame();
    check(editor->presetShown() == "one", "presets: 'save' writes it and clears the mark");

    //  ---- one click loads, the host's parameters included, as one undo step ---------------------
    base.renameInstance("kept name");
    check(editor->clickPresetRow(kDefaultPreset), "presets: a row takes a click");
    frame();
    check(near(plain(), home), "presets: a load sets the HOST's parameter, not only the patch");
    check(base.document().editing().matrix.empty(), "presets: a load replaces the matrix");
    check(editor->presetShown() == "default", "presets: the header names what was loaded");
    check(base.identity().label == "kept name", "presets: a load leaves who the instance is");
    check(editor->presetsOpen(), "presets: the browser stays open after a load, to try the next");

    check(editor->clickUndo(), "presets: a load can be undone from the header");
    frame();
    check(near(plain(), away) && base.document().editing().matrix.size() == 1,
          "presets: undoing a load brings back the parameters AND the patch, in one step");
    check(editor->presetShown() == "one", "presets: and the name of the preset that was there");
    check(editor->clickUndo(true), "presets: and redone");
    frame();
    check(near(plain(), home) && editor->presetShown() == "default", "presets: redoing a load loads it again");

    //  a parameter moved after the load is what an undo then a redo comes back to
    setPlain(away);
    editor->clickUndo();
    editor->clickUndo(true);
    frame();
    check(near(plain(), away), "presets: a redo comes back to what was there when the load was undone");
    setPlain(home);

    //  ---- the chevrons step through the whole list, whatever the search says -------------------
    //  a third preset, so that forward and back from `default` are not the same place
    base.presets().save({false, "", "aaa"}, base.documentState());
    browser.typeSearch("zzz");
    frame();
    check(browser.rowsShown().empty(), "presets: a search with no hit lists nothing");
    check(editor->clickPresetStep(1), "presets: the header's chevron takes a click");
    frame();
    const auto next = bambi::stepPreset(base.presets().all(), kDefaultPreset, 1);  // a room in Reverb, "aaa" elsewhere
    check(base.currentPreset() == next && !(next == kDefaultPreset),
          "presets: a step forward ignores the search and lands on the NEXT in the list");
    check(editor->clickPresetStep(-1), "presets: and back");
    frame();
    check(editor->presetShown() == "default", "presets: a step back returns");

    //  ---- search: flat hits with the folder as a tag; arrows and return; escape twice ----------
    browser.typeSearch("CHECKS");
    frame();
    {
        const auto rows = browser.rowsShown();
        check(rows.size() == 2 && rows[1].ref == one && rows[1].tag == "checks",
              "presets: a search matches the folder, whatever the case, and lists the hit with its folder");
    }
    browser.pressKey(juce::KeyPress(juce::KeyPress::returnKey));
    frame();
    check(editor->presetShown() == "one", "presets: return loads the row the cursor is on");
    browser.pressKey(juce::KeyPress(juce::KeyPress::escapeKey));
    check(editor->presetsOpen() && browser.rowsShown().size() > 2, "presets: a first escape clears the search");
    browser.pressKey(juce::KeyPress(juce::KeyPress::escapeKey));
    check(!editor->presetsOpen(), "presets: a second escape closes the browser");

    //  ---- rename moves the file, and the folder it emptied goes ---------------------------------
    editor->openPresets(true);
    frame();
    check(editor->clickPresetAction(Action::Rename) && browser.nameOffered() == "checks/one",
          "presets: rename offers the whole path, folder included");
    browser.typeName("moved/two");
    frame();
    const PresetRef two{false, "moved", "two"};
    check(fs::exists(root / "moved" / "two.json") && !fs::exists(root / "checks"),
          "presets: a rename moves the file, and the folder left empty goes");
    check(editor->presetShown() == "two", "presets: the header follows a rename, unmodified");

    //  ---- a name that is taken: rename refuses, save as asks ------------------------------------
    editor->clickPresetAction(Action::SaveAs);
    browser.typeName("other");
    frame();
    editor->clickPresetAction(Action::Rename);
    browser.typeName("moved/two");
    check(browser.naming() && browser.noteShown().isNotEmpty(),
          "presets: a rename onto a taken name is refused, and says so");
    browser.pressKey(juce::KeyPress(juce::KeyPress::escapeKey));
    editor->openPresets(true);
    frame();

    const float third = (home + away) / 2.0f;  // "two" holds `away`, "other" holds `home`
    setPlain(third);
    frame();
    editor->clickPresetAction(Action::SaveAs);
    browser.typeName("moved/two");
    check(editor->questionShown() && editor->questionAsked().contains("replace"),
          "presets: saving over ANOTHER preset asks first");
    check(editor->clickAnswer(false) && !editor->questionShown(), "presets: the question takes a no");
    browser.typeName("moved/two");
    editor->pressKeyOnQuestion(juce::KeyPress(juce::KeyPress::escapeKey));
    check(!editor->questionShown(), "presets: and escape is a no");
    {
        const auto still = base.presets().load(two);
        check(still.has_value() && near(still->params[static_cast<std::size_t>(at)], away),
              "presets: and a no leaves that preset as it was");
    }
    browser.typeName("moved/two");
    editor->clickAnswer(true);
    frame();
    {
        const auto now = base.presets().load(two);
        check(
            now.has_value() && near(now->params[static_cast<std::size_t>(at)], third) && editor->presetShown() == "two",
            "presets: a yes replaces it, and it is the current one");
    }

    //  ---- delete asks; the sound stays -----------------------------------------------------------
    check(editor->clickPresetAction(Action::Delete) && editor->questionShown(), "presets: delete asks first");
    editor->clickAnswer(false);
    check(fs::exists(root / "moved" / "two.json"), "presets: a no deletes nothing");
    editor->clickPresetAction(Action::Delete);
    editor->clickAnswer(true);
    frame();
    check(!fs::exists(root / "moved" / "two.json") && !fs::exists(root / "moved"),
          "presets: a yes removes the file and its emptied folder");
    check(near(plain(), third), "presets: deleting a preset leaves the sound as it is");
    check(editor->presetShown() == "two *", "presets: a patch whose preset is gone shows as modified: it is unsaved");

    //  ---- folding is remembered, and a load unfolds what it lands in ---------------------------
    check(editor->clickPresetHeading("factory") && base.presets().folded("factory"),
          "presets: a heading folds at a click");
    frame();
    check(browser.rowArea(kDefaultPreset).isEmpty(), "presets: and what is under it is not listed");
    editor->clickPresetStep(1);   // from a preset that is gone, which counts as the first: to a user preset
    editor->clickPresetStep(-1);  // and back, into the folded group
    frame();
    check(base.currentPreset() == kDefaultPreset && !base.presets().folded("factory") &&
              !browser.rowArea(kDefaultPreset).isEmpty(),
          "presets: a load unfolds the heading it lands under");

    //  ---- a key a preset leaves alone --------------------------------------------------------------
    if (!spec.kept.empty()) {
        const int keptAt = m.byKey(spec.kept);
        auto* kept = base.hostParameter(static_cast<ParamId>(keptAt));
        kept->setValueNotifyingHost(1.0f);
        const float before = kept->getValue();
        editor->clickPresetRow(kDefaultPreset);
        frame();
        check(kept->getValue() == before,
              "presets: '" + juce::String(spec.kept) + "' is what is plugged in, and a load leaves it");
        check(editor->presetShown() == "default", "presets: and it does not count as modified either");
    }

    //  ---- the session carries the name; one page at a time ----------------------------------------
    editor->clickPresetAction(Action::SaveAs);
    browser.typeName("kept in the session");
    juce::MemoryBlock saved;
    proc.getStateInformation(saved);
    editor->openSettings(true);
    check(!editor->presetsOpen(), "presets: the settings page closes the browser: one page at a time");
    window.reset();
    {
        Processor reopened;
        reopened.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        host::PluginProcessor& again = reopened;
        check(again.currentPreset() == PresetRef{false, "", "kept in the session"} && !again.presetModified(),
              "presets: a session reopened names its preset, unmodified");
    }

    fs::remove_all(scratch);
    host::PluginProcessor::usePresetDirectory({});
}

}  // namespace bambi::editor
