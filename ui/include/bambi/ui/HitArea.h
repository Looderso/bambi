// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <vector>

namespace bambi::ui {

/*  A panel that rebuilds its clickable areas every paint, and scrolls under a part that stays put.
    Shared by every plugin: it knows nothing about a patch, a parameter or a processor -- only about
    rectangles, the mouse and a scroll offset.

    Regions are rebuilt on every paint at the coordinates they are drawn at, so shifting where the
    content starts moves the click areas with it and nothing else has to know. A panel declares its
    viewport and what it drew, during paint. */
class HitArea : public juce::Component {
public:
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    /// After every paint, which has just measured the content: keeps the position within reach.
    void paintOverChildren(juce::Graphics& g) override;

protected:
    /// A panel with more content than window scrolls under a part that stays put -- the matrix's column
    /// heads, the right panel's tab bar. The panel declares its viewport and what it drew, during paint.
    void setViewport(float top, float bottom) {
        viewTop_ = top;
        viewBottom_ = bottom;
        contentBottom_ = top;
    }
    /*  Slack only once something actually overflows. Added unconditionally it manufactures travel for a
        panel whose content fits, which is how the default three matrix rows came to scroll. */
    float maxScroll() const {
        const auto over = contentBottom_ - viewBottom_;
        return over > 0.0f ? over + kScrollPad : 0.0f;
    }

public:
    /*  Click, and drag, exactly where the mouse would -- through the painted regions and their
     *  handlers, not by setting state. For tools and for checks: a UI that can only be exercised by
     *  a person is a UI that is not checked.
     *
     *  The caller paints first: regions are built while painting, as they are in a real window. */
    void clickAt(juce::Point<float> at);
    /// Double-click exactly where the mouse would: a parameter back to its default.
    void doubleClickAt(juce::Point<float> at);
    /// Press at `from`, move to `to`, release. `fine` is the shift-drag.
    void dragBetween(juce::Point<float> from, juce::Point<float> to, bool fine = false);
    /*  The same gesture in its three parts, for a check that has to look while the mouse is still
        down: a plot whose scale is frozen for the duration of a drag can only be wrong mid-drag,
        and a check that paints after the release sees the settled picture. */
    void pressAt(juce::Point<float> at, bool fine = false);
    void moveTo(juce::Point<float> to, bool fine = false);
    void releaseDrag();

    /// How far this panel can travel, and where it is. For tools.
    float scrollRange() const { return maxScroll(); }
    float scrollPosition() const { return scroll_; }
    /// Scroll as the wheel does, clamped the same way. For tools.
    void scrollTo(float y) {
        const auto next = std::clamp(y, 0.0f, scrollRange());
        if (!juce::exactlyEqual(next, scroll_)) {
            scroll_ = next;
            repaint();
        }
    }

    /*  Scroll until `drawn` -- a box as the last paint drew it -- is inside the viewport, as a user
        does before reaching for something below the fold. True when it moved, and then the caller
        paints again: what is hidden takes no press, so a tool cannot click what a person could not. */
    bool scrollIntoView(juce::Rectangle<float> drawn) {
        const auto before = scroll_;
        if (drawn.getBottom() > viewBottom_)
            scrollTo(scroll_ + drawn.getBottom() - viewBottom_);
        else if (drawn.getY() < viewTop_)
            scrollTo(scroll_ - (viewTop_ - drawn.getY()));
        return !juce::exactlyEqual(before, scroll_);
    }

    /*  What a page drew, and where it is scrolled to. Public because a shared drawing helper reports
     *  the same things a panel's own paint does -- a source's settings page is drawn by `ui/` and
     *  scrolls in whichever plugin's panel it lands in. */
    void noteContentBottom(float y) { contentBottom_ = std::max(contentBottom_, y); }
    float scroll() const { return scroll_; }

    /*  The modifiers the press carried. `Region::press` takes only a position, and a gesture that
     *  branches on alt or shift at press time -- the encoder's scene breaks a node's symmetry with
     *  alt and deletes with shift -- needs them. Read from the event and not from
     *  `ModifierKeys::getCurrentModifiers()`, which reports the real keyboard: a synthetic event
     *  built by a check carries its own, and the live keyboard would ignore them.
     *
     *  Valid inside `press`, `drag`, `release` and `click`; it is the last press's. */
    const juce::ModifierKeys& pressModifiers() const { return pressMods_; }

    /// A click and nothing else, public so a shared widget can register its own. No `drag`, which is
    /// the point: a press on a corner toggle must not orbit the globe under it. Later regions lie on
    /// top, so one added after a view's whole-area region takes the press from it.
    void addClickArea(juce::Rectangle<float> area, std::function<void()> click) {
        addRegion({area, {}, {}, {}, std::move(click), {}});
    }

    /// A whole gesture, public for the same reason: a shared widget that is dragged -- the envelope's
    /// graph, whose points and curve handles are pulled about -- registers press, move and release,
    /// not a click.
    void addDragArea(juce::Rectangle<float> area, std::function<void(juce::Point<float>)> press,
                     std::function<void(juce::Point<float>, bool)> drag, std::function<void()> release) {
        addRegion({area, std::move(press), std::move(drag), std::move(release), {}, {}});
    }

    /// Everything a value takes: press, move and release, a click when the press never became a
    /// drag, and a double-click. Public so that one shared statement of how a host parameter is
    /// moved can register it on any panel (`HostDrag.h`).
    void addGestureArea(juce::Rectangle<float> area, std::function<void(juce::Point<float>)> press,
                        std::function<void(juce::Point<float>, bool)> drag, std::function<void()> release,
                        std::function<void()> click, std::function<void()> doubleClick) {
        addRegion(
            {area, std::move(press), std::move(drag), std::move(release), std::move(click), std::move(doubleClick)});
    }

protected:
    static constexpr float kScrollPad = 14.0f;  ///< slack, so a last line is never what gets cut

    struct Region {
        juce::Rectangle<float> area;
        std::function<void(juce::Point<float>)> press;
        std::function<void(juce::Point<float>, bool)> drag;  ///< position, fine (shift held)
        std::function<void()> release;
        std::function<void()> click;
        std::function<void()> doubleClick;
    };

    void clearRegions() { regions_.clear(); }
    void addRegion(Region region) {
        //  what the viewport hides takes no press: a tile scrolled under the tab bar is not there
        if (hitClip_.has_value()) region.area = region.area.getIntersection(*hitClip_);
        regions_.push_back(std::move(region));
    }

public:
    /*  The scrolled content, as against the part that stays put. While one of these lives, what is
     *  drawn is clipped to the viewport and so is what is registered: a region is registered where
     *  it is drawn, later regions lie on top, and content scrolled under the pinned part would
     *  otherwise take the press from the tab or the column head drawn over it. One object for
     *  both, so a panel cannot clip the picture and forget the mouse. After `setViewport`. */
    class ContentClip {
    public:
        ContentClip(HitArea& area, juce::Graphics& g);
        ~ContentClip();

    private:
        HitArea& area_;
        juce::Graphics& g_;
        JUCE_DECLARE_NON_COPYABLE(ContentClip)
    };

private:
    juce::ModifierKeys pressMods_;
    juce::Point<float> pressedAt_;
    std::optional<juce::Point<float>> movedTo_;
    float scroll_{0.0f};
    float contentBottom_{0.0f}, viewTop_{0.0f}, viewBottom_{0.0f};
    std::vector<Region> regions_;
    std::optional<juce::Rectangle<float>> hitClip_;  ///< set while a ContentClip lives
    std::optional<Region> active_;                   ///< a copy: a repaint mid-drag rebuilds regions_
    bool dragged_{false};
};

}  // namespace bambi::ui
