// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

/*  The live host's own column, left of the plugin it shows: the views to switch between -- a kind of plugin, or
 *  the routing page -- and at its foot what the host hears from Live, and what went wrong when something did. The plugin's window keeps the only header;
 *  the column leaves the header's height free and continues its line, and a strong rule parts it from the plugin.
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

    /// The views, the open one, what a click on one does. A label after an empty one stands apart from those
    /// before it.
    void setViews(std::vector<juce::String> labels, int open, std::function<void(int)> select);
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
};

}  // namespace bambi::livehost
