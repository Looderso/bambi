// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "bambi/ui/HitArea.h"
#include "bambi/ui/TextField.h"

/*  The settings page, the same frame in every plugin. It opens from the header's glyph over the
 *  scene, the matrix and the tabs, and leaves the level column and the footer where they are:
 *  always on screen, and a settings page is no exception.
 *
 *  Where the shared frame stops: the left column is what every instance of every plugin has --
 *  name, session, host order -- and the right column is the plugin's own, handed over as choices,
 *  actions and lines of readout. The page knows no plugin.
 *
 *  It is this window's own instance, whichever one the controls are showing: a name is identity,
 *  not patch, and the way to name another instance is its own window.
 */
namespace bambi::ui {

struct SettingsContent {
    juce::String name;       ///< the user's own; empty while the track's is used
    juce::String trackName;  ///< what the host calls the track: shown greyed in an empty name
    bool linked{false};
    juce::String session;
    int peers{0};
    int order{-1};

    /// One of a few, as segments: Reverb's "when rendering".
    struct Choice {
        juce::String label;
        std::vector<juce::String> options;
        int selected{0};
        std::function<void(int)> choose;
        juce::String note;   ///< a line under it, when it needs one
        bool enabled{true};  ///< false draws it greyed and inert
    };
    /// Something to open or to run: the encoder's host readout and timing trace.
    struct Action {
        juce::String label, button;
        std::function<void()> run;
    };
    std::vector<Choice> own;
    std::vector<Action> actions;
    std::vector<std::pair<juce::String, juce::String>> readout;  ///< label, value
};

/// The shared frame's half of what a plugin answers, from any processor that is on the link bus --
/// named by what it needs, not by a type, because `ui/` does not depend on `host/`.
template <typename Processor>
SettingsContent sharedSettings(Processor& processor, int order) {
    SettingsContent c;
    const auto link = processor.linkStatus();
    c.name = juce::String(processor.identity().label);
    c.trackName = processor.linkNode().trackName();
    c.linked = link.open;
    c.session = link.session;
    c.peers = link.peers;
    c.order = order;
    return c;
}

/// What an effect's host has told it, as readout rows: the same three in Echo and in Reverb.
template <typename Diagnostics>
void addHostReadout(SettingsContent& c, const Diagnostics& d) {
    c.readout.push_back({"sample rate", juce::String(d.sampleRate.load(), 0) + " Hz"});
    c.readout.push_back({"channels", juce::String(d.mainInputChannels.load()) + " in, " +
                                         juce::String(d.outputChannels.load()) + " out, " +
                                         juce::String(d.sidechainChannels.load()) + " sidechain"});
    c.readout.push_back({"transport", !d.transportKnown.load() ? juce::String("none from the host")
                                      : d.playing.load()       ? juce::String("playing")
                                                               : juce::String("stopped")});
}

class SettingsPage final : public HitArea {
public:
    SettingsPage(juce::String plugin, std::function<SettingsContent()> content,
                 std::function<void(const juce::String&)> rename, std::function<void()> close);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Show it over the window, or hide it. The footer comes in front of it: it is always on screen.
    void open(bool show, juce::Component& footer);
    /// Once a frame: repaint when what it shows has changed under it. Does nothing while closed.
    void refresh();
    /// How often that has asked for a repaint. For checks: a repaint is not observable.
    int refreshesAsked() const { return refreshes_; }

    /*  Type a name and commit it, as return does; and where things were drawn. For checks: a text
     *  field is the one control here that a press alone cannot drive. */
    void typeName(const juce::String& text);
    juce::String nameShown() const { return name_.getText(); }
    juce::Rectangle<float> closeArea() const { return close_; }
    juce::Rectangle<float> optionArea(int choice, int option) const;
    /// Where the button of the `index`-th action was last drawn; empty when there is no such action.
    juce::Rectangle<float> actionArea(int index) const {
        return index >= 0 && index < static_cast<int>(actions_.size()) ? actions_[static_cast<std::size_t>(index)]
                                                                       : juce::Rectangle<float>{};
    }

private:
    void commitName();

    juce::String plugin_;
    std::function<SettingsContent()> content_;
    std::function<void(const juce::String&)> rename_;
    std::function<void()> closeNow_;
    TextField name_;
    juce::Rectangle<float> close_;
    juce::String shown_;  ///< what the last refresh found, to know when to repaint
    int refreshes_{0};
    std::vector<std::vector<juce::Rectangle<float>>> options_;
    std::vector<juce::Rectangle<float>> actions_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsPage)
};

}  // namespace bambi::ui
