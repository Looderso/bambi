// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

#include <algorithm>

#include "bambi/encode/params.hpp"
#include "bambi/encode/pathparams.hpp"
#include "bambi/ui/RegionEditor.h"

// ---- what is built before the shared window ----------------------------------------------------

bambi::ui::EncoderWindowParts::EncoderWindowParts(BambiEncoderProcessor& p)
    : controls(p),
      trace(p),
      globe(state, bambi::Projection::Globe),
      equirect(state, bambi::Projection::Equirect),
      tabs(controls, state),
      diagnostics(p, trace, [this] {
          trace.start();
          diagnostics.setVisible(false);
      }) {
    controls.selectSelf = [this, &p] { state.select(p.identity().instance); };
    globe.onPaint = [this] { trace.painted(); };

    /*  The region in the scene, where the engine has it: the parameters as set, turned by what its
        rates have added since -- read from the audio thread's own count, as the effects read theirs.
        Another instance's turn is not on the bus, so one selected from here is drawn as set. `open`
        while its source page is showing, which is the only time the encoder has a region tab open
        at all. */
    bambi::ui::RegionHook hook;
    const auto turnBy = [this, &p](std::size_t axis) {
        return controls.remote ? 0.0
                               : static_cast<double>(p.diagnostics().regionTurn[axis].load(std::memory_order_relaxed));
    };
    hook.regions = [this, turnBy](std::vector<bambi::ui::SceneRegion>& into) {
        const auto region = bambi::ui::regionOf(controls, controls.regionSlot(), turnBy(0), turnBy(1), turnBy(2));
        if (region.kind == bambi::RegionKind::Everywhere)
            return;  // everywhere is the whole sphere: an edge round all of it says nothing
        into.push_back(
            {region, juce::String(controls.regionSlot().role), controls.sourceOpen(bambi::kNumSources - 1), true});
    };
    //  what the engine plays beside it, and the other encoders', as they resolved them
    const auto own = hook.regions;
    hook.regions = [this, own, turnBy](std::vector<bambi::ui::SceneRegion>& into) {
        own(into);
        bambi::ui::appendLiveRegion(
            scene, state.selected, 0,
            bambi::ui::regionOf(controls, controls.regionSlot(), turnBy(0), turnBy(1), turnBy(2)), into);
        bambi::ui::appendOtherRegions(scene, bambi::Product::Encoder, state.selected, into);
    };
    //  A handle carried or double-clicked is written back as the effects write theirs: through the slot's
    //  parameters, with the turn a rate has added taken off.
    hook.carried = [this, turnBy](bambi::ui::RegionHandle::Kind kind, bambi::Vec3 to) {
        const auto& slot = controls.regionSlot();
        const double yaw = turnBy(0), pitch = turnBy(1), roll = turnBy(2);
        bambi::ui::carryRegionHandle(controls, slot, bambi::ui::regionOf(controls, slot, yaw, pitch, roll), kind, to,
                                     yaw, pitch, roll);
    };
    hook.reset = [this, turnBy](bambi::ui::RegionHandle::Kind kind) {
        const auto& slot = controls.regionSlot();
        bambi::ui::resetRegionHandle(controls, slot,
                                     bambi::ui::regionOf(controls, slot, turnBy(0), turnBy(1), turnBy(2)), kind);
    };
    globe.setRegionHook(hook);
    equirect.setRegionHook(hook);

    //  Node edits from the scene go the same way every other edit does: through the undo stack of whichever
    //  instance the controls are on, which is what makes editing another instance's path work.
    state.editNodes = [this](const juce::String& name, const juce::String& key,
                             std::function<void(bambi::TrajectoryState&)> change) {
        const auto apply = [change](bambi::PluginState& s) { change(s.trajectory); };
        if (key.isEmpty())
            controls.edit(name.toStdString(), apply);
        else
            controls.editDrag(name.toStdString(), key.toStdString(), apply);
    };
    state.endNodeEdit = [this] { controls.endDrag(); };
}

// ---- the editor --------------------------------------------------------------------------------

BambiEncoderEditor::BambiEncoderEditor(BambiEncoderProcessor& p)
    : bambi::ui::EncoderWindowParts(p),
      bambi::editor::PluginEditor(p, {controls, state, tabs, globe, equirect, state}, "encoder",
                                  {{bambi::EncoderParam::RenderGain, "gain"}},
                                  {{bambi::EncoderParam::RatesRetrigger, "rates continue"}}),
      processor_(p) {
    state.changed = [this] { sceneChanged(); };
    state.userAction = [this] { processor_.noteUserActivity(); };
    equirect.setRelayout([this] { relayout(); });
    addOverlay(diagnostics);
    tick();  // a first frame before the first tick
}

void BambiEncoderEditor::addOwnSettings(bambi::ui::SettingsContent& c) {
    //  the host readout and the scene's timing trace are the encoder's own, and stay a page of their own
    c.actions.push_back({"host readout and timing trace", "open", [this] {
                             showSettings(false);
                             diagnostics.setVisible(true);
                             diagnostics.toFront(false);
                             footerToFront();
                         }});
}

bool BambiEncoderEditor::updateSelection() {
    const auto* selected = state.find(state.selected);
    const auto* peer = selected != nullptr && !selected->self ? scene.find(state.selected) : nullptr;
    const bool remote = peer != nullptr;
    const bool moved = remote != controls.remote || (remote && !(peer->instance == controls.remoteTarget.instance()));
    if (moved) {
        //  What was open on the instance the controls are leaving ends there: a learn, a drag, the provisional row.
        controls.stopLearning();
        controls.remoteTarget.release();
        controls.provisional = bambi::kNoParamId;
    }
    controls.remote = remote;
    if (remote)
        controls.remoteTarget.follow(
            *peer, 1.0 / bambi::ui::kFrameHz);  // every frame: its controls, and what its sources send

    const auto label = selected != nullptr ? selected->label : juce::String();
    const bool ready = controls.target().ready();
    const bool changed = moved || label != controls.selectedLabel || ready != ready_;
    controls.selectedLabel = label;
    ready_ = ready;
    return changed;
}

void BambiEncoderEditor::updateLivePath() {
    bambi::ui::SceneInstance* shown = nullptr;
    for (auto& instance : state.instances) {
        instance.livePath.clear();
        if (instance.id == state.selected) shown = &instance;
    }
    const auto& patch = controls.patch;
    //  a chain is played as drawn
    if (shown == nullptr || patch.trajectory.kind != bambi::TrajectoryKind::Parametric) return;
    auto set = patch.trajectory;
    bambi::pathSettingsFrom(set, patch.params);
    auto live = set;
    bool differs = false;
    for (int i = 0; i < bambi::kMaxGenParams; ++i) {
        const auto id = bambi::pathParamId(set.generator, i);
        double engine = 0.0;
        //  the path's settings are parameters: what its engine published for them
        if (id == bambi::kNoParamId || !controls.engineValue(id, engine)) continue;
        const auto at = static_cast<std::size_t>(i);
        live.genParams[at] = engine;
        //  half a degree, round the circle: the live one crossed the bus as floats
        differs = differs || std::abs(std::remainder(live.genParams[at] - set.genParams[at], 360.0)) > 0.5;
    }
    if (!differs) return;
    if (live.generator != liveBuiltFrom_.generator || live.genParams != liveBuiltFrom_.genParams || livePath_.empty()) {
        livePath_.build(live);
        liveBuiltFrom_ = live;
    }
    //  through the placement as it plays, about the live path's own centre, at the drawn path's density
    const auto& points = livePath_.points();
    const auto centre = bambi::pathCentre(points);
    const std::size_t step = std::max<std::size_t>(1, points.size() / static_cast<std::size_t>(bambi::kLinkPathPoints));
    for (std::size_t k = 0; k < points.size(); k += step)
        shown->livePath.push_back(bambi::applyTransform(points[k], centre, shown->transform));
}

void BambiEncoderEditor::frame(bambi::ui::EditorFrame& parts) {
    //  a held region handle, re-applied every frame: a turning region slips from under a still pointer
    globe.carryHeld();
    equirect.carryHeld();
    processor_.updateScene(scene);
    state.rebuild(scene, processor_.identity().instance, bambi::Product::Encoder);

    trace.frame(scene);  // only while a timing trace runs: right after the editor read the bus
    const auto& d = processor_.diagnostics();
    //  Nodes are edited where they mean something: the trajectory tab, a custom chain, and an extent that
    //  can be undone -- below kEditableExtent the path is collapsed onto its centre and a drag has no way back.
    const auto& trajectory = controls.patch.trajectory;
    const auto* shown = state.find(state.selected);
    const bool editable = controls.tab == 0 && trajectory.kind == bambi::TrajectoryKind::Custom &&
                          trajectory.nodes.size() >= 2 && shown != nullptr &&
                          shown->transform.extent >= bambi::kEditableExtent;
    if (editable) state.chain = trajectory;
    if (editable != state.editing) {
        state.editing = editable;
        if (!editable) {
            state.selectedNode = -1;
            state.hoveredNode = -1;
            state.hoveredHandle = false;
            state.insert = {};
        }
    }

    const bool selection = updateSelection();
    controls.liveScene = &scene;  // what its engine plays: the tiles' live marks
    controls.shownInstance = state.selected;
    const bool changed = controls.refresh() || selection;
    updateLivePath();

    const auto link = processor_.linkStatus();
    const auto* shownInstance = state.find(state.selected);
    parts.refresh({{link.open, link.session, link.peers},
                   controls.target().canUndo(),
                   controls.target().canRedo(),
                   juce::String(controls.target().undoName()),
                   juce::String(controls.target().redoName()),
                   changed,
                   d.outputPeak.load(),
                   shownInstance != nullptr ? shownInstance->label : juce::String(),
                   d.order.load(),
                   processor_.dspLoad()});
    globe.repaint();
    equirect.repaint();
    if (diagnostics.isVisible()) diagnostics.repaint();
}

void BambiEncoderEditor::sceneChanged() {
    globe.repaint();
    equirect.repaint();
    //  The immediate path a click takes. The frame refreshes too, but a host that throttles timers
    //  would leave the open tab behind until it resumed, and this costs nothing when nothing changed.
    header().refresh();
    const bool selection = updateSelection();
    //  a node picked in the scene opens its settings in the trajectory tab at once, not at the first drag
    const bool node = state.selectedNode != shownNode_;
    shownNode_ = state.selectedNode;
    if (selection || node) {
        matrix().repaint();
        tabs.repaint();
    }
}

void BambiEncoderEditor::showControls(int tab, bambi::MatrixTab matrixTab, bambi::ParamId provisional) {
    controls.tab = std::clamp(tab, 0, 2);
    controls.matrixTab = matrixTab;
    controls.provisional = provisional;
    controls.notify();
}

void BambiEncoderEditor::showNode(int index) {
    state.selectedNode = index;
    tick();
}

bambi::Vec3 BambiEncoderEditor::sceneNodePosition(int index) const {
    const auto& chain = state.chain;
    if (index < 0 || index >= static_cast<int>(chain.nodes.size())) return {};
    const auto* shown = state.find(state.selected);
    if (shown == nullptr) return {};
    //  a node is authored untransformed and drawn where the transform puts it
    return bambi::applyTransform(chain.nodes[static_cast<std::size_t>(index)].p, shown->centre, shown->transform);
}

bool BambiEncoderEditor::clickViewPreset(int index) {
    paintNow();
    const auto box = globe.presetArea(index);
    if (box.isEmpty()) return false;
    globe.clickAt(box.getCentre());
    return true;
}

void BambiEncoderEditor::clickScene(bambi::Vec3 at, bool onGlobe) {
    paintNow();
    auto& view = onGlobe ? globe : equirect;
    view.clickAt(view.screenOf(at));
}

void BambiEncoderEditor::dragScene(bambi::Vec3 from, float dx, float dy, bool onGlobe) {
    paintNow();
    auto& view = onGlobe ? globe : equirect;
    const auto start = view.screenOf(from);
    view.dragBetween(start, start.translated(dx, dy));
}

void BambiEncoderEditor::dragSceneSteps(bambi::Vec3 from, float dx, float dy, int steps, bool onGlobe) {
    paintNow();
    auto& view = onGlobe ? globe : equirect;
    const auto start = view.screenOf(from);
    const auto at = [&](int step) {
        return start.translated(dx * static_cast<float>(step) / static_cast<float>(steps),
                                dy * static_cast<float>(step) / static_cast<float>(steps));
    };
    const auto now = juce::Time::getCurrentTime();
    const auto event = [&](juce::Point<float> p) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(), 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, &view, &view, now, start, now, 1, false);
    };
    view.mouseDown(event(start));
    for (int step = 1; step <= steps; ++step) view.mouseDrag(event(at(step)));
    view.mouseUp(event(at(steps)));
}

void BambiEncoderEditor::setEquirectFull(bool full) {
    state.equirectFull = full;
    relayout();
    tick();
}

void BambiEncoderEditor::clickFull() {
    paintNow();
    const auto box = equirect.fullToggleArea();
    if (!box.isEmpty()) equirect.clickAt(box.getCentre());
}

void BambiEncoderEditor::clickRegionsAlways() {
    paintNow();
    equirect.clickAt(equirect.regionsToggleArea().getCentre());
}

void BambiEncoderEditor::dragOffRegionsAlways(float dx) {
    paintNow();
    const auto from = equirect.regionsToggleArea().getCentre();
    equirect.dragBetween(from, from.translated(dx, 0.0f));
}

bool BambiEncoderEditor::touchSourceStep(bool forward) {
    paintNow();
    const auto area = tabs.stepArea(forward);
    if (area.isEmpty()) return false;
    const auto at = area.getCentre();
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, &tabs, &tabs, now, at, now, 1, false);
    tabs.mouseDown(e);
    tabs.mouseUp(e);
    return true;
}

void BambiEncoderEditor::selectInstance(const bambi::Uuid& instance) {
    state.select(instance);
    tick();
}

void BambiEncoderEditor::chooseRegionKind(int kind) {
    controls.edit("region kind", [kind](bambi::PluginState& s) {
        auto& r = s.regions[0].shape;
        r.kind = static_cast<bambi::RegionKind>(std::clamp(kind, 0, 4));
        r = bambi::sanitised(r);
    });
    tick();
}

bool BambiEncoderEditor::liveMark(bambi::ParamId id, double& out) {
    //  Paint first: the live region is built while painting, as it is in the plugin window.
    paintNow();
    if (!tabs.liveValue(id, out)) return false;
    return tabs.hasLiveRegion();
}

juce::Rectangle<float> BambiEncoderEditor::liveMarkBounds() {
    paintNow();
    return tabs.liveBounds();
}
