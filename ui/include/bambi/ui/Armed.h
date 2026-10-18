// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "bambi/ui/Draw.h"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/Widgets.h"

namespace bambi::ui {

/*  A pick waiting for the user's next click -- "send" for where its wire goes, a sidechain slot for its source,
    "+ copy" for its channel -- drawn and answered one way on every page. The control that began it is drawn as a
    block in the waiting colour saying what it waits for and that escape cancels; a click on it, on empty ground or
    escape lets go. Everything it may take is ringed, dashed, and answers a click; everything else on the page that
    could have been clicked is laid under the ground, out of reach. `What` is what waits: whose, and for what.

    The page calls ground() before it draws, then block() in place of the control, over() on each box once drawn,
    and escape() from its key handler. Message thread. */
template <typename What>
class Armed {
public:
    bool on() const { return what_.has_value(); }
    const std::optional<What>& what() const { return what_; }
    void begin(What what) { what_ = std::move(what); }
    void end() { what_.reset(); }

    /// Under everything the page draws after it: a click on empty ground lets go.
    void ground(HitArea& page, juce::Rectangle<float> all, std::function<void()> changed) {
        if (on())
            page.addClickArea(all, [this, then = std::move(changed)] {
                end();
                if (then) then();
            });
    }

    /// How wide the block is for what it says.
    static float width(const juce::String& waitsFor) { return framedButtonWidth(label(waitsFor)); }

    /// The block, in place of the control that began it; a click on it lets go.
    void block(HitArea& page, juce::Graphics& g, juce::Rectangle<float> box, const juce::String& waitsFor,
               std::function<void()> changed) {
        drawWaiting(g, box, label(waitsFor), smallFont());
        page.addClickArea(box, [this, then = std::move(changed)] {
            end();
            if (then) then();
        });
    }

    /// Over a box once it is drawn, while something waits: ringed and answering when it takes, else out of reach.
    void over(HitArea& page, juce::Graphics& g, juce::Rectangle<float> box, bool takes, std::function<void()> take) {
        if (!on()) return;
        if (!takes) {
            drawOutOfReach(g, box);
            return;
        }
        drawTakes(g, box);
        page.addClickArea(box, std::move(take));
    }

    /// Escape: lets go; true when something was waiting.
    bool escape() {
        if (!on()) return false;
        end();
        return true;
    }

private:
    static juce::String label(const juce::String& waitsFor) { return waitsFor + kSeparator + "esc cancels"; }
    std::optional<What> what_;
};

}  // namespace bambi::ui
