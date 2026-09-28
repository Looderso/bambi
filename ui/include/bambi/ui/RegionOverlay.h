// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "bambi/link/link.hpp"
#include "bambi/region/shape.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/Theme.h"

/// A region in the scene: the tab says what a region is, this is where it is seen and driven.
/// Drawn in ink as a wash over the side that passes and its 0.5 contour, sampled through `valueAt`,
/// the region's own oracle. Handles are in world space and hit-tested in screen space, the aim last
/// so a handle sitting on top of it is taken first.
namespace bambi::ui {

struct RegionHandle {
    enum class Kind { Edge, Elevation, Roll, Aim };
    Kind kind{Kind::Aim};
    Vec3 direction{0.0, 0.0, 1.0};
    /// Where it is drawn and hit on the equirect: the direction itself, except the aim at a pole,
    /// which sits at the azimuth its angles give (or the roll stem's, when there is one).
    Vec3 onMap{0.0, 0.0, 1.0};

    Vec3 shownIn(Projection projection) const { return projection == Projection::Equirect ? onMap : direction; }
};

/// Where a region's aim is drawn in `projection`: on the equirect, by `aimOnMap`, so a pole holds still.
Vec3 aimShownIn(const Region& r, Projection projection);

/// The handles of a region, as applied. "Everywhere" has none: there is nothing to aim and nothing to size.
void regionHandles(const Region& r, std::vector<RegionHandle>& into);

/// The reference dot of a set of dots, in the region's own frame: the one nearest the axis that is
/// not on it. The roll and size handles are carried from it. Empty for a pair.
std::optional<Vec3> referenceDot(int dots);

/// One region as the scene shows it. A closed slot shows its edge and role label and no wash; an
/// open one shows edge, wash and handles, with the energy under it drawn at half.
struct SceneRegion {
    Region region;
    juce::String role;  ///< "send", "return": what a closed slot is labelled by
    bool open{false};   ///< its tab is the one showing: wash and handles
    bool own{true};     ///< false is another instance's: silver, dotted, edge only
    bool live{false};   ///< where the engine has it now, beside the set one: a thin live-coloured edge

    bool operator==(const SceneRegion&) const = default;
};

/// The handles over it. `held` is the one being dragged and `hovered` the one under the pointer,
/// both drawn full; -1 for none.
void paintRegionHandles(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
                        const std::vector<RegionHandle>& handles, int held = -1, int hovered = -1);

/// Which handle a press at `at` takes, or -1. On the globe only what faces the viewer can be hit.
int regionHandleAt(Projection projection, const Camera& camera, const Viewport& vp,
                   const std::vector<RegionHandle>& handles, juce::Point<float> at);

/// The regions a plugin has, and how a handle carried somewhere is written back. Only the open slot
/// takes handles and its wash. `carried` and `reset` may be empty, leaving the regions drawn but not driven.
struct RegionHook {
    /// The regions to draw, in order. The one marked `open` takes handles; at most one is.
    std::function<void(std::vector<SceneRegion>& into)> regions;
    /// A handle was carried to `to`. The plugin writes it back through its own parameters, so it
    /// automates and undoes like anything else.
    std::function<void(RegionHandle::Kind kind, Vec3 to)> carried;
    /// A handle was double-clicked: put what it drives back to its default.
    std::function<void(RegionHandle::Kind kind)> reset;
};

/// Ask the hook what to draw. `into` is the caller's, so a paint allocates nothing; it is cleared
/// and refilled. Returns the index of the open region, or -1. `always` false keeps only the open
/// one; it defaults on because with it off, the blue is still shaped by a region nobody can see.
int gatherRegions(const RegionHook& hook, std::vector<SceneRegion>& into, bool always = true);

/// The other instances' regions, of this window's own kind, appended to `into` as another
/// instance's -- silver, dotted, edge only, never open. `shown`, the instance whose own regions the
/// window draws from its controls, is left out, as is another kind's regions (a Reverb's send is
/// where the field enters its room, not a place for a source).
void appendOtherRegions(const LinkScene& scene, Product kind, const Uuid& shown, std::vector<SceneRegion>& into);

/// What the engine plays, beside what is set: slot `slot` of the shown instance as its audio thread
/// resolved it this step, appended as a live outline where it differs from `set`.
void appendLiveRegion(const LinkScene& scene, const Uuid& shown, int slot, const Region& set,
                      std::vector<SceneRegion>& into);

/// A scene view that shows and drives a region: the encoder's sphere and the effects' probe views
/// both are one, so one set of checks drives either.
class RegionView {
public:
    virtual ~RegionView() = default;
    virtual const std::vector<RegionHandle>& handles() const = 0;
    virtual const std::vector<SceneRegion>& regionsDrawnNow() const = 0;
    /// Where a direction is on screen, in this view's own pixels.
    virtual juce::Point<float> screenOf(Vec3 direction) const = 0;
    virtual Projection projection() const = 0;
    juce::Point<float> pixelOf(const RegionHandle& h) const { return screenOf(h.shownIn(projection())); }
};

/// A view's grip on the open region's handles: which were drawn, which one is held, and carrying it.
/// `carry` runs every drag and once a frame, since a turning region slips from under a still pointer.
class RegionGrip {
public:
    /// After the regions are painted: the open one's handles, drawn and remembered.
    void paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
               const std::vector<SceneRegion>& regions, int open);
    /// Which handle is under `at`, or -1.
    int at(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> at) const;
    /// Take the press when it is on a handle. True when one is held.
    bool press(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> at);
    bool held() const { return held_ >= 0 && held_ < static_cast<int>(handles_.size()); }
    /// Carry the held handle to where the pointer last was, `at` if given. True when it moved anything.
    bool carry(const RegionHook& hook, Projection projection, const Camera& camera, const Viewport& vp);
    void moveTo(juce::Point<float> at) { pointer_ = at; }
    void release() { held_ = -1; }
    /// The pointer moved with nothing pressed: the handle under it is drawn as if held, with a grab
    /// cursor. True when which one changed, so the view repaints.
    bool hover(Projection projection, const Camera& camera, const Viewport& vp, juce::Point<float> at);
    bool unhover();
    bool hovering() const { return hovered_ >= 0; }
    /// A double-click: the handle under `at` back to its default. True when there was one.
    bool reset(const RegionHook& hook, Projection projection, const Camera& camera, const Viewport& vp,
               juce::Point<float> at) const;

    const std::vector<RegionHandle>& handles() const { return handles_; }
    int heldIndex() const { return held_; }

private:
    std::vector<RegionHandle> handles_;
    int held_{-1};
    int hovered_{-1};
    juce::Point<float> pointer_{};  ///< where the pointer last was, re-applied every frame
    juce::Point<float> grab_{};     ///< the handle's offset from the pointer when it was taken
};

/// The regions of one view, drawn and remembered: a region is sampled through a `RegionField` on a
/// grid in screen space, which is what lets one routine draw every kind. The wash is kept as an
/// image, a pixel a cell, and the contour as a path, both already cut to the sphere, until the
/// region moves.
class RegionLayer {
public:
    void paint(juce::Graphics& g, Projection projection, const Camera& camera, const Viewport& vp,
               const std::vector<SceneRegion>& regions);

    /// How many times a region has been sampled, as against drawn from what was kept. For tools.
    int builds() const { return builds_; }

private:
    struct Built {
        bool valid{false};
        SceneRegion scene;
        juce::Image wash;                 ///< a pixel a cell; null when nothing passes on screen
        juce::Rectangle<float> washArea;  ///< where the image goes, in the view's pixels
        juce::Path edge;                  ///< the 0.5 contour, already dashed for another instance's
    };
    struct Grid {
        bool valid{false};
        Projection projection{Projection::Globe};
        Camera camera;
        Viewport vp;
        int left{0}, top{0}, wide{1}, tall{1};
        std::vector<Vec3> directions;  ///< (wide + 1) x (tall + 1), a row at a time
        juce::Rectangle<int> frame;    ///< the view's own rectangle: what a paint is clipped to
    };
    void lookThrough(Projection projection, const Camera& camera, const Viewport& vp);
    void build(Built& into, const SceneRegion& scene);

    Grid grid_;
    std::vector<Built> built_;
    std::vector<float> above_, here_;
    int builds_{0};
};

}  // namespace bambi::ui
