// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

/*  The live host's own column, left of the plugin or page it shows. In the header's height at its top, Live's
 *  transport: play and stop. Under it the kinds of plugin to switch between; at its foot the host's own pages, and
 *  under them what the host hears from Live, and what went wrong when something did. It continues the plugin's header
 *  line, and a strong rule parts it from the plugin.
 *
 *  Message thread.
 */
namespace bambi::livehost {

class Rail final : public juce::Component {
public:
    struct Status {
        bool on{false};                   ///< the square lit: Live is connected and heard
        std::vector<juce::String> lines;  ///< short lines, the first beside the square
        juce::String problem;             ///< what went wrong, if anything: wrapped above the lines
    };

    /// The views, the open one, what a click on one does. The labels after the first empty one stand at the
    /// column's foot, above its status.
    void setViews(std::vector<juce::String> labels, int open, std::function<void(int)> select);
    /// Whether Live plays, as the top of the column shows it.
    void setPlaying(bool playing);
    /// A click on play (true), or on stop while Live plays (false).
    std::function<void(bool)> transport;
    /// A click on stop while Live is stopped: back to the song's start, as Live's own stop does.
    std::function<void()> toStart;
    /// Space: play when Live is stopped, stop when it plays. False for any other key, which passes on.
    bool takeKey(const juce::KeyPress& key);
    /// A press at `at`, as a click there does: on play, stop or a view. For checks too.
    void pressAt(juce::Point<float> at);
    /// Where play and stop were last drawn. For checks.
    juce::Rectangle<float> drawnPlay() const { return play_; }
    juce::Rectangle<float> drawnStop() const { return stop_; }
    /// The column's width: its widest possible view name, every plugin's and the host's own pages, with its padding.
    static int width();
    void setStatus(Status status);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    std::vector<juce::String> labels_;
    int open_{0};
    std::function<void(int)> select_;
    std::vector<juce::Rectangle<float>> areas_;
    Status status_;
    bool playing_{false};
    juce::Rectangle<float> play_, stop_;
};

}  // namespace bambi::livehost
