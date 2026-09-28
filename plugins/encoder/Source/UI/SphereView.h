// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

#include "UI/SceneModel.h"
#include "bambi/scene/view.hpp"
#include "bambi/ui/EnergyLayer.h"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/RegionOverlay.h"
#include "bambi/ui/SceneStrip.h"

namespace bambi::ui {

/*  One view of the scene: the globe or the equirect. Every instance's path and source, the selected one
    in blue and orange, the others in silver. On the globe a drag orbits the view and the presets snap it;
    a click on a dot or a path selects that instance. A source is never dragged. A custom chain's nodes are:
    while the shell has editing on, a handle bends, a node moves, a click on the curve inserts and
    shift+click deletes -- each an undoable edit of the selected instance, so it works on another instance too.

    Geometry, hit-testing and the click-or-drag rule are bambi/view.hpp, tested there; this draws and
    forwards events. */
class SphereView final : public HitArea, public bambi::ui::RegionView {
public:
    SphereView(SceneState& state, bambi::Projection projection);

    /*  The region over the sphere: the encoder's region gates a source, so seeing where it sits against
        the path is the whole point of showing it. Its handles drive it while its page is open, as the
        effects' spheres do. */
    void setRegionHook(bambi::ui::RegionHook hook) { region_ = std::move(hook); }
    /*  Re-apply the held handle, if there is one. Once a frame from the editor's tick: a region turning
        under a rate slips from under a still pointer otherwise. */
    void carryHeld();
    bool handleHeld() const { return grip_.held(); }
    const std::vector<bambi::ui::RegionHandle>& handles() const override { return grip_.handles(); }
    /// Called when a corner toggle changed the shape of the scene, so the editor lays it out again.
    void setRelayout(std::function<void()> relayout) { relayout_ = std::move(relayout); }

    /// How many regions the last paint drew. Recorded while painting, for checks.
    int regionsDrawn() const { return static_cast<int>(regions_.size()); }
    int regionBuilds() const { return regionLayer_.builds(); }
    /// The regions the last paint drew, turned as it drew them. For checks.
    const std::vector<bambi::ui::SceneRegion>& regionsDrawnNow() const override { return regions_; }
    bambi::Projection projection() const override { return projection_; }

    /// Where a direction lands in this view's pixels, through its own projection. For checks.
    juce::Point<float> screenOf(bambi::Vec3 direction) const override;
    /// Where the corner toggles were last drawn. Empty on the globe. For checks.
    juce::Rectangle<float> fullToggleArea() const { return fullToggle_; }
    juce::Rectangle<float> energyToggleArea() const { return energyToggle_; }
    juce::Rectangle<float> regionsToggleArea() const { return regionsToggle_; }
    bool energyDrawn() const { return energyDrawn_; }

    void paint(juce::Graphics& g) override;
    /*  Press, drag, click and their 4 px slop are `HitArea`'s: the scene registers what it drew and no
        longer carries mouse handlers of its own. Hover is still here, because `HitArea` has none --
        nothing else in the suite hovers. */
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    std::function<void()> onPaint;  ///< diagnostics: called as painting starts

private:
    bambi::Viewport viewport() const;
    const SceneInstance* editable() const;                 ///< the instance whose chain is being edited, or none
    void chainNodes(std::vector<bambi::Vec3>& out) const;  ///< the chain's nodes where they are drawn
    void paintChain(juce::Graphics& g, const bambi::Viewport& vp);
    /// True when the press was an edit and not a view move; `mods` is the press's (alt breaks, shift deletes).
    bool beginNodeGesture(juce::Point<float> at, const juce::ModifierKeys& mods);
    void dragNode(juce::Point<float> at);
    bool hoverChain(juce::Point<float> at);  ///< true while the chain owns the pointer
    SceneStripAreas strip_{};                ///< where the strip drew its controls, this paint

public:
    /// Where the globe's `index`-th view preset was drawn -- top, front, side; empty on the equirect.
    juce::Rectangle<float> presetArea(int index) const {
        return index >= 0 && index < 3 ? strip_.presets[static_cast<std::size_t>(index)] : juce::Rectangle<float>{};
    }

private:
    void buildRegions();  ///< what the scene drew, as click and drag areas; called at the end of paint
    /*  Where the `regions always` toggle was last drawn, on the equirect only. Empty on the globe.
        Recorded by the paint, so `mouseDown` tests where it really is. */
    juce::Rectangle<float> regionsToggle_{}, fullToggle_{}, energyToggle_{};
    bambi::ui::EnergyLayer energyLayer_;  ///< the picture, as every scene view draws it
    bool energyDrawn_{false};
    std::function<void()> relayout_;

    int instanceAt(float x, float y) const;  ///< index into state_.instances, or -1
    void notify();
    void strokeSegments(juce::Graphics& g, juce::Colour colour, float width, bool dashed) const;

    SceneState& state_;
    bambi::ui::RegionHook region_;
    std::vector<bambi::ui::SceneRegion> regions_;
    bambi::ui::RegionLayer regionLayer_;
    bambi::ui::RegionGrip grip_;  ///< the open region's handles, and the one held
    bambi::Projection projection_;
    juce::Point<float> pressedAt_;
    bool pressConsumed_{false};  ///< the chain took the press, so no select
    enum class Drag { None, Node, Handle };
    Drag drag_{Drag::None};
    int dragIndex_{-1};
    bambi::Handle dragWhich_{bambi::Handle::In};
    mutable std::vector<bambi::Vec3> nodePoints_;         ///< reused by every hit test and paint
    mutable std::vector<bambi::ScreenSegment> segments_;  ///< reused every frame
    mutable std::vector<juce::Point<double>> skipped_;    ///< points a stroke has passed over, reused every frame
    bambi::CapCoverage coverage_;                         ///< the selected source's width, reused every frame
    juce::Image widthImage_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SphereView)
};

}  // namespace bambi::ui
