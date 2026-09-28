// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/HitArea.h"

#include "bambi/scene/view.hpp"  // kClickSlopPixels: what counts as a drag rather than a click

namespace bambi::ui {

void HitArea::mouseDown(const juce::MouseEvent& e) {
    active_.reset();
    dragged_ = false;
    pressMods_ = e.mods;
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)  // later regions lie on top
    {
        if (it->area.contains(e.position)) {
            active_ = *it;
            break;
        }
    }
    if (active_ && active_->press) active_->press(e.position);
}

void HitArea::mouseDrag(const juce::MouseEvent& e) {
    if (!active_) return;
    //  on the screen, not in the component: the window's scale is a transform, and 4 of its units are
    //  2 on screen at 0.5x
    if (!dragged_ && e.getScreenPosition().toDouble().getDistanceFrom(e.getMouseDownScreenPosition().toDouble()) >
                         bambi::kClickSlopPixels)
        dragged_ = true;
    if (dragged_ && active_->drag) active_->drag(e.position, e.mods.isShiftDown());
}

void HitArea::mouseUp(const juce::MouseEvent&) {
    if (!active_) return;
    const Region region = *active_;
    active_.reset();
    if (region.release) region.release();
    if (!dragged_ && region.click) region.click();
}

void HitArea::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    //  Only below the pinned part, and only when there is somewhere to go: a wheel that moves nothing reads
    //  as a broken control, and one that scrolls the heading away defeats pinning it.
    constexpr float kWheelPixels = 140.0f;
    const auto limit = maxScroll();
    if (limit <= 0.0f || e.position.y < viewTop_) return;
    const auto next = std::clamp(scroll_ - wheel.deltaY * kWheelPixels, 0.0f, limit);
    if (!juce::exactlyEqual(next, scroll_)) {
        scroll_ = next;
        repaint();
    }
}

void HitArea::paintOverChildren(juce::Graphics&) {
    //  content that shrinks under a scrolled panel -- a node deselected, a tab switched, a section
    //  hidden -- can leave the scroll position past the new end, and a panel that no longer overflows
    //  takes no wheel event to scroll it back up with. Every paint measures what it drew, so clamping
    //  here catches every case regardless of what caused the shrink.
    if (const auto limit = maxScroll(); scroll_ > limit) {
        scroll_ = limit;
        repaint();
    }
}

HitArea::ContentClip::ContentClip(HitArea& area, juce::Graphics& g) : area_(area), g_(g) {
    const juce::Rectangle<float> view{0.0f, area.viewTop_, static_cast<float>(area.getWidth()),
                                      std::max(0.0f, area.viewBottom_ - area.viewTop_)};
    g.saveState();
    g.reduceClipRegion(view.getSmallestIntegerContainer());
    area.hitClip_ = view;
}

HitArea::ContentClip::~ContentClip() {
    area_.hitClip_.reset();
    g_.restoreState();
}

void HitArea::mouseDoubleClick(const juce::MouseEvent& e) {
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
        if (it->area.contains(e.position)) {
            if (it->doubleClick) it->doubleClick();
            return;
        }
    }
}

namespace {
juce::MouseEvent eventAt(juce::Component& on, juce::Point<float> at, juce::Point<float> from, bool fine) {
    const auto mods = fine ? juce::ModifierKeys(juce::ModifierKeys::shiftModifier) : juce::ModifierKeys::noModifiers;
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &on, &on, juce::Time::getCurrentTime(), from, juce::Time::getCurrentTime(), 1, false);
}
}  // namespace

void HitArea::clickAt(juce::Point<float> at) {
    const auto e = eventAt(*this, at, at, false);
    mouseDown(e);
    mouseUp(e);
}

void HitArea::doubleClickAt(juce::Point<float> at) { mouseDoubleClick(eventAt(*this, at, at, false)); }

void HitArea::dragBetween(juce::Point<float> from, juce::Point<float> to, bool fine) {
    pressAt(from, fine);
    moveTo(to, fine);
    releaseDrag();
}

void HitArea::pressAt(juce::Point<float> at, bool fine) {
    pressedAt_ = at;
    mouseDown(eventAt(*this, at, at, fine));
}

void HitArea::moveTo(juce::Point<float> to, bool fine) {
    movedTo_ = to;
    mouseDrag(eventAt(*this, to, pressedAt_, fine));
}

void HitArea::releaseDrag() {
    mouseUp(eventAt(*this, movedTo_.value_or(pressedAt_), pressedAt_, false));
    movedTo_.reset();
}

}  // namespace bambi::ui
