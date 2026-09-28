// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

#include "bambi/ui/InstanceModel.h"
#include "bambi/ui/Theme.h"

namespace bambi::ui {

/*  The header: wordmark and product, undo and redo, the instances, preset, link status, settings.

    Selecting an instance lives here rather than in a list beside the scene. The selected tab is open
    at the bottom -- the header's rule breaks under it -- so what every panel below is showing reads at
    a glance. They scroll only when they cannot all fit, since a session may hold up to
    kLinkMaxInstances of them. */
class Header final : public juce::Component, public juce::TooltipClient {
public:
    /// `plugin` is the word after the wordmark -- "encoder", "echo", "reverb". Everything else is shared.
    Header(InstanceModel& instances, juce::String plugin, std::function<void()> onSettings,
           std::function<void()> onUndo, std::function<void()> onRedo);
    /// What the link line reads. Three fields and not `host::LinkStatus`: `ui` does not depend on `host`.
    struct Link {
        bool open{false};
        juce::String session;
        int peers{0};
    };
    void setStatus(Link status);
    /*  The preset cluster, `< preset . name* >`: the name opens the browser, the chevrons step through
        the whole list. Greyed and inert while the window shows another instance -- a preset is this
        instance's own. Repaints when it changes. */
    struct Preset {
        juce::String name;
        bool modified{false};
        bool open{false};  ///< the browser is showing: the name is drawn as an open tab is
        bool available{true};

        bool operator==(const Preset&) const = default;
    };
    void setPreset(Preset preset);
    /// The icon and the name are home: a click takes the window back to its own instance, whichever it
    /// is showing. On its own instance it does nothing.
    void onHome(std::function<void()> home) { onHome_ = std::move(home); }
    /// Where the icon, the wordmark and the plugin's name were drawn. For a check that clicks them.
    juce::Rectangle<float> homeArea() const { return homeArea_; }
    void onPreset(std::function<void()> toggleBrowser, std::function<void(int)> step);
    /// Where the name and the two chevrons were last drawn. For a check that clicks them as the mouse does.
    juce::Rectangle<float> presetNameArea() const { return presetName_; }
    juce::Rectangle<float> presetStepArea(bool forward) const { return forward ? presetNext_ : presetPrev_; }
    juce::String presetShown() const { return preset_.name + (preset_.modified ? " *" : ""); }
    /// Whether undo and redo can run, and what each would revert or reapply (their tooltips).
    void setUndoState(bool canUndo, bool canRedo, juce::String undoName = {}, juce::String redoName = {});
    /// "undo set width" over an enabled undo, and the same for redo; nothing elsewhere.
    juce::String getTooltip() override;
    /// Where undo and redo were last drawn. For a check that clicks them as the mouse does.
    juce::Rectangle<float> undoArea() const { return undoBounds_; }
    juce::Rectangle<float> redoArea() const { return redoBounds_; }
    /// The settings page is open: the glyph is drawn as an open tab is. Repaints when it changes.
    void setSettingsOpen(bool open);
    /// The plugin's own colour, for the rule under the header and round its open tab. Set once.
    void setIdentity(juce::Colour c) {
        identity_ = c;
        repaint();
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    /*  Repaint if what the header would draw no longer matches what it last drew -- the selected instance,
        the names, how many there are. None of that is on a clock, and nothing else repaints the header.
        Returns whether it asked. */
    bool refresh();
    /// How many times refresh() has had to ask for a redraw. For tools.
    int repaintsAsked() const { return asked_; }

private:
    juce::Colour identity_ = theme::colour::identity(Product::Encoder);
    struct Tab {
        bambi::Uuid id;
        juce::String label;
        juce::Rectangle<float> area;
    };
    juce::Rectangle<float> settingsBounds() const;
    juce::Rectangle<float> iconBounds() const;
    /// The plugin's mark, in its reserved square before the wordmark.
    void paintIcon(juce::Graphics& g) const;
    /// Lay the instances across whatever middle the header has left, scrolling only if they overflow it.
    void layoutTabs(juce::Rectangle<float> middle);

    InstanceModel& instances_;
    juce::String plugin_;
    std::function<void()> onSettings_, onUndo_, onRedo_;
    bool settingsOpen_{false};
    Link status_;
    Preset preset_;
    std::function<void()> onPresetName_;
    std::function<void()> onHome_;
    juce::Rectangle<float> homeArea_;
    std::function<void(int)> onPresetStep_;
    juce::Rectangle<float> presetName_, presetPrev_, presetNext_;
    bool canUndo_{false}, canRedo_{false};
    juce::String undoName_, redoName_;
    juce::Rectangle<float> undoBounds_, redoBounds_;
    std::vector<Tab> tabs_;
    juce::Rectangle<float> tabArea_, prevChevron_, nextChevron_;
    std::size_t paintedCount_{0};
    int asked_{0};
    float tabScroll_{0.0f}, tabMaxScroll_{0.0f};
    bambi::Uuid lastSelected_{};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Header)
};

}  // namespace bambi::ui
