// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <vector>

#include "bambi/scene/view.hpp"
#include "bambi/ui/EnergyLayer.h"
#include "bambi/ui/HitArea.h"
#include "bambi/ui/RegionOverlay.h"
#include "bambi/ui/SceneStrip.h"

/*  The sphere, in whichever projection: the energy, the probe, and what the plugin answers.
 *
 *  One mechanism for Echo's ping and Reverb's early-reflection overlay alike: a direction in, an
 *  indicator out, supplied as a list of marks -- where each arrives from, how strongly, and how
 *  sure of itself it is. Everything else -- graticule, measured energy, the probe and its
 *  interaction (a click places it, a drag orbits) -- is here, drawn in the added colour so it reads
 *  as the same thing the measured blue will become.
 */
namespace bambi::ui {

/// One bead: where a thing arrives from, how loud, and how wide a patch of sky it arrives from. A
/// repeat arrives from a widening patch rather than a point, so the spread is a soft sprite drawn
/// fainter as it widens -- otherwise the widest, quietest late passes are all that is left visible.
struct ProbeMark {
    Vec3 direction{1.0, 0.0, 0.0};
    double weight{1.0};     ///< linear, against 1: drawn as opacity
    double spreadRad{0.0};  ///< the patch of sky it arrives from. 0 is a point
    int group{0};           ///< which of the plugin's answers this belongs to -- a tap, say
    bool lead{false};       ///< this group is the selected one: drawn in full, the rest at a third
};

/// A curve on the sphere, with the fade running along it as a per-point weight rather than one for
/// the whole line. The far side of the globe is drawn too, dimmed, so a curve never breaks in half.
struct ProbeTrace {
    std::vector<Vec3> points;
    std::vector<float> weight;  ///< per point, linear; empty draws the whole curve at `lead`'s alpha
    int group{0};
    bool lead{false};
    bool faint{false};  ///< the flow, which is context rather than the thing being read
};

/// What the view asks of the plugin. Called while painting, on the message thread. `into` is
/// cleared first; an empty answer draws nothing but the probe itself.
struct ProbeReply {
    std::vector<ProbeMark> marks;
    std::vector<ProbeTrace> traces;
    bool numbered{true};  ///< each group a numbered answer in its own colour; false draws all in the added colour
};
using ProbeAnswer = std::function<void(Vec3 from, ProbeReply& into)>;

class ProbeView final : public HitArea, public RegionView {
public:
    /// How the scene is looked at -- camera, preset, `regions always`, `full` -- is `SceneViewState`;
    /// what is the probe's own is below.
    struct State : SceneViewState {
        Vec3 at{1.0, 0.0, 0.0};  ///< where it is pointed
        bool placed{false};      ///< a user put it there, rather than it never having been placed
    };

    ProbeView(State& state, Projection projection, ProbeAnswer answer, std::function<void()> changed);

    /// What region to draw over the sphere, and what a handle does. Optional.
    void setRegionHook(RegionHook hook) { region_ = std::move(hook); }
    /// Called when a corner toggle changed the shape of the scene, so the editor lays it out again.
    void setRelayout(std::function<void()> relayout) { relayout_ = std::move(relayout); }

    /// Where a corner toggle was last drawn, and how many regions the last paint drew. Recorded
    /// while painting, so a check clicks where the toggle really is and reads what was really
    /// drawn. Empty / 0 on the globe, which has no corner. For tools.
    juce::Rectangle<float> regionsToggleArea() const { return regionsToggle_; }
    juce::Rectangle<float> energyToggleArea() const { return energyToggle_; }
    juce::Rectangle<float> fullToggleArea() const { return fullToggle_; }
    /// Where the globe's `index`-th view preset was drawn -- top, front, side; empty on the equirect.
    juce::Rectangle<float> presetArea(int index) const {
        return index >= 0 && index < 3 ? strip_.presets[static_cast<std::size_t>(index)] : juce::Rectangle<float>{};
    }
    int regionsDrawn() const { return static_cast<int>(regions_.size()); }
    /// The regions the last paint drew, turned as it drew them. For checks.
    const std::vector<SceneRegion>& regionsDrawnNow() const override { return regions_; }
    juce::Point<float> screenOf(Vec3 direction) const override;
    Projection projection() const override { return projection_; }
    bool energyDrawn() const { return energyDrawn_; }

    /// Which handle a press would take, and where they are. For tools.
    const std::vector<RegionHandle>& handles() const override { return grip_.handles(); }
    int handleAt(juce::Point<float> at) const;

    /// Re-apply the held handle, if there is one. Called once a frame by the editor's tick: a region
    /// turning under a rate slips from under a still pointer otherwise. Does nothing when no handle
    /// is held, which is almost always.
    void carryHeld();
    bool handleHeld() const { return grip_.held(); }

    /// A region handle under the pointer is drawn as if held, with a grab cursor.
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    /// Once a frame, from the editor's tick, after it has re-read the patch. Repaints when a region
    /// is no longer where it was drawn: one turning under a rate moves with nothing on screen
    /// having been touched, and so does one a host automates.
    void followRegions();
    /// How many times a region has been sampled rather than drawn from what was kept. For tools.
    int regionBuilds() const { return regionLayer_.builds(); }
    /// How many times a moved region has asked for a repaint. For checks: a repaint is not observable.
    int regionRepaintsAsked() const { return regionRepaintsAsked_; }

    void paint(juce::Graphics& g) override;

    /// Place it as a click does, from a normalised position in [-1, 1]. False when that is off the sphere.
    bool placeAt(double nx, double ny);
    /// Where a normalised position falls, in this view's own pixels. For tools.
    juce::Point<float> pixelsFor(double nx, double ny) const;

private:
    Viewport viewport() const;
    /// The globe fills a 128 x 64 RGBA texture and resamples it bilinearly through a table rebuilt
    /// only when the camera turns, rather than drawing a quad per texel.
    void paintFrame(juce::Graphics& g, const Viewport& vp);
    void paintAxisLabels(juce::Graphics& g, const Viewport& vp);
    void paintEnergy(juce::Graphics& g, const Viewport& vp);
    void paintGraticule(juce::Graphics& g, const Viewport& vp);
    void paintAnswer(juce::Graphics& g, const Viewport& vp);
    /// A path whose opacity changes along it. There is no gradient along an arbitrary path, so the
    /// segments are sorted into a handful of opacity bands and each band strokes once.
    void strokeFaded(juce::Graphics& g, const Viewport& vp, const ProbeTrace& trace);
    void paintBead(juce::Graphics& g, const Viewport& vp, const ProbeMark& mark);
    void paintProbe(juce::Graphics& g, const Viewport& vp);
    void paintRegions(juce::Graphics& g, const Viewport& vp);

    State& state_;
    Projection projection_;
    ProbeAnswer answer_;
    std::function<void()> changed_;
    ProbeReply reply_;
    RegionHook region_;
    std::function<void()> relayout_;
    std::vector<ScreenSegment> segments_, lineSegments_;  ///< a stroke's segments, reused every paint
    std::vector<juce::Point<double>> skipped_;            ///< the stroker's scratch, likewise
    std::vector<SceneRegion> regions_;
    std::vector<SceneRegion> regionsNow_;  ///< what the hook answers this frame, against what was drawn
    RegionLayer regionLayer_;
    int regionRepaintsAsked_{0};
    int openRegion_{-1};  ///< which of `regions_` takes handles, or -1
    /// The open region's handles and the one held. Its grab offset keeps a handle from jumping to
    /// the cursor on the first drag frame -- without it a 4 px slop is also a 4 px shove.
    RegionGrip grip_;
    bool pressOnHandle_{false};  ///< the press took a handle: its release places nothing
    juce::Rectangle<float> regionsToggle_{}, energyToggle_{}, fullToggle_{};
    SceneStripAreas strip_{};  ///< where the strip drew its controls, this paint
    bool energyDrawn_{false};
    EnergyLayer energyLayer_;  ///< the picture, as every scene view draws it
    juce::Point<float> pressedAt_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProbeView)
};

}  // namespace bambi::ui
