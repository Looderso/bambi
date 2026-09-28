// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_core/juce_core.h>
#include <vector>

#include "bambi/link/link.hpp"
#include "bambi/path/authoring.hpp"
#include "bambi/scene/view.hpp"
#include "bambi/ui/InstanceModel.h"
#include "bambi/ui/SceneStrip.h"

namespace bambi::ui {

/// One instance as the scene draws it: its path with the live transform applied, and its source.
struct SceneInstance {
    bambi::Uuid id;
    juce::String label;
    bool self{false};
    bool hasPosition{false};
    bambi::Vec3 position{1.0, 0.0, 0.0};
    double s{0.0};  ///< where on its path, 0..1, after the movement mode: displace's live mark
    /*  What a stereo input puts on the sphere: 0 sum, 1 mid/side, 2 stereo, and its two points.
        `position` stays where the source is -- what a click selects and the region source reads. */
    int inputMode{0};
    bambi::Vec3 left{1.0, 0.0, 0.0}, right{1.0, 0.0, 0.0};
    double widthRad{0.0};  ///< as rendered, modulation included: the cap's angular radius
    std::vector<bambi::Vec3> path;
    bool closed{true};
    /*  Where the path is, as the engine built it this step, when that differs from the set one: drawn
        beside it, thin. Empty otherwise. Only the selected instance's is filled. */
    std::vector<bambi::Vec3> livePath;

    //  What the drawn path already has applied, so a node can be drawn through it and a drag brought back.
    bambi::Vec3 centre{0.0, 0.0, 1.0};
    bambi::PathTransform transform{};
};

/*  What every scene view draws and edits together: the instances, the selected and hovered one, and
    the globe's camera. Rebuilt from the link bus at the editor's frame rate; message thread only.
    Selection is this window's, not the session's -- another window may have another one selected. */
struct SceneState : public InstanceModel, public SceneViewState {
    //  ---- InstanceModel: what the shared header asks ----------------------------------------
    int instanceCount() const override { return static_cast<int>(instances.size()); }
    bambi::Uuid instanceAt(int index) const override {
        return index >= 0 && index < instanceCount() ? instances[static_cast<std::size_t>(index)].id : bambi::Uuid{};
    }
    juce::String instanceLabel(int index) const override {
        return index >= 0 && index < instanceCount() ? instances[static_cast<std::size_t>(index)].label
                                                     : juce::String();
    }
    bambi::Uuid selectedInstance() const override { return selected; }
    void selectInstance(const bambi::Uuid& id) override { select(id); }

    std::vector<SceneInstance> instances;
    bambi::Uuid selected;
    bambi::Uuid hovered;

    /*  Editing the selected instance's custom chain, in the scene. The shell turns it on while the
        trajectory tab is open on a custom path, and hands over the chain the controls hold; the views draw
        nodes and handles only then, and every change goes back out through `editNodes` -- so it is undoable,
        and works on another instance exactly as it does on this one. */
    bool editing{false};
    //  the camera, the preset and the scene corner's two switches are `SceneViewState`'s
    bambi::TrajectoryState chain;
    int selectedNode{-1};
    int hoveredNode{-1};
    bool hoveredHandle{false};
    bambi::Handle hoveredWhich{bambi::Handle::In};
    bambi::CurveHit insert;  ///< where a click would insert, for the preview
    juce::String notice;     ///< a refusal the user has to see -- the node cap
    double noticeUntilMs{0.0};

    /// One edit of the chain: an undo name, a coalescing key (empty for a single step), and the change.
    std::function<void(const juce::String&, const juce::String&, std::function<void(bambi::TrajectoryState&)>)>
        editNodes;
    /// The drag is over: close the run of coalescing edits.
    std::function<void()> endNodeEdit;

    /// Say something in the scene for a moment -- a refusal, never a status.
    void say(const juce::String& text);

    std::function<void()> changed;     ///< something a view shows has changed: repaint them
    std::function<void()> userAction;  ///< the user did something: counts as session activity

    const SceneInstance* find(const bambi::Uuid& id) const;

    /// Refresh from the bus. A selection that has gone falls back to this instance.
    /*  The scene, from what the bus is carrying. Only instances of `product` are kept: one bus
     *  carries all three plugins, and an instance in this list is one this window can select and
     *  edit -- which across kinds is meaningless, because a parameter position means a different
     *  parameter in a different plugin. An effect has no source to draw either. */
    void rebuild(const bambi::LinkScene& scene, const bambi::Uuid& self, bambi::Product product);

    void select(const bambi::Uuid& id);
};

}  // namespace bambi::ui
